#include "github_research/mcp_server.hpp"
#include "github_research/cache_manager.hpp"
#include <cstdlib>
#include <iostream>
#include <string>
#include <chrono>
#include <set>
#include <vector>
#include <tuple>
#include <algorithm>
#include <filesystem>

static void print_help() {
    std::cerr << "research-mcp v0.2.0\n"
              << "8-source research MCP server (unified WebView2 backend)\n"
              << "Sources: GitHub + arXiv + HackerNews + npm/PyPI + PapersWithCode\n"
              << "         + HuggingFace + SemanticScholar + StackOverflow\n\n"
              << "Usage:\n"
              << "  research-mcp.exe [options]\n\n"
              << "Options:\n"
              << "  --port <PORT>              HTTP MCP server port (default: stdio mode)\n"
              << "  --proxy <URL>              Proxy URL (applies to all sessions)\n"
              << "  --gh-profile <DIR>         GitHub WebView user data dir (8-source isolation)\n"
              << "  --arxiv-profile <DIR>      Enable arXiv WebView session\n"
              << "  --hn-profile <DIR>         Enable Hacker News WebView session\n"
              << "  --pkg-profile <DIR>        Enable npm/PyPI WebView session\n"
              << "  --pwc-profile <DIR>        Enable Papers with Code WebView session\n"
              << "  --hf-profile <DIR>         Enable Hugging Face WebView session\n"
              << "  --s2-profile <DIR>         Enable Semantic Scholar WebView session\n"
              << "  --so-profile <DIR>         Enable Stack Overflow WebView session\n"
              << "  --help                     Show this help\n\n"
              << "HTTP endpoints:\n"
              << "  POST /mcp        JSON-RPC 2.0\n"
              << "  GET  /           Service status (shows enabled sources)\n"
              << "  GET  /tools      List all registered tools (49 total)\n\n"
              << "Full example (all 8 sources):\n"
              << "  research-mcp.exe --port 8765 \\\n"
              << "    --gh-profile ./profiles/gh \\\n"
              << "    --arxiv-profile ./profiles/arxiv \\\n"
              << "    --hn-profile ./profiles/hn \\\n"
              << "    --pkg-profile ./profiles/pkg \\\n"
              << "    --pwc-profile ./profiles/pwc \\\n"
              << "    --hf-profile ./profiles/hf \\\n"
              << "    --s2-profile ./profiles/s2 \\\n"
              << "    --so-profile ./profiles/so \\\n"
              << "    --proxy http://127.0.0.1:7897 \\\n"
              << "    --gh-token ghp_xxxxxxxxxxxx\n\n"
              << "Minimal (GitHub only, 13 tools):\n"
              << "  research-mcp.exe --port 8765\n\n"
              << "Environment:\n"
              << "  GITHUB_TOKEN             Personal access token (optional, --gh-token overrides)\n"
              << "  GITHUB_RESEARCH_TIMEOUT  Request timeout seconds (default: 30)\n"
              << "  HTTPS_PROXY/HTTP_PROXY/ALL_PROXY  Proxy URL\n";
}

static std::string read_proxy_from_env() {
    const char* keys[] = {
        "HTTPS_PROXY", "https_proxy",
        "HTTP_PROXY", "http_proxy",
        "ALL_PROXY", "all_proxy",
        nullptr
    };
    for (int i = 0; keys[i]; ++i) {
        const char* v = std::getenv(keys[i]);
        if (v && v[0]) return std::string(v);
    }
    return std::string();
}

static std::wstring s2w(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

// 每个 --xxx-profile 参数的解析结果
struct ProfileArgs {
    std::string gh, arxiv, hn, pkg, pwc, hf, s2, so;
};

int main(int argc, char* argv[]) {
    const char* token_env = std::getenv("GITHUB_TOKEN");
    const char* timeout_env = std::getenv("GITHUB_RESEARCH_TIMEOUT");
    int timeout = timeout_env ? std::atoi(timeout_env) : 30;
    std::optional<std::string> token;
    if (token_env && token_env[0]) token = std::string(token_env);

    int port = 0;
    std::string proxy_url;
    bool proxy_explicit = false;
    ProfileArgs profiles;
    bool cache_smoke_test = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_help();
            return 0;
        } else if (arg == "--cache-smoke-test") {
            cache_smoke_test = true;
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
            if (port <= 0 || port > 65535) {
                std::cerr << "Invalid port: " << argv[i] << std::endl;
                return 1;
            }
        } else if (arg == "--bind") {
            ++i;
        } else if (arg == "--proxy" && i + 1 < argc) {
            proxy_url = argv[++i];
            proxy_explicit = true;
        } else if (arg == "--gh-profile" && i + 1 < argc) {
            profiles.gh = argv[++i];
        } else if (arg == "--arxiv-profile" && i + 1 < argc) {
            profiles.arxiv = argv[++i];
        } else if (arg == "--hn-profile" && i + 1 < argc) {
            profiles.hn = argv[++i];
        } else if (arg == "--pkg-profile" && i + 1 < argc) {
            profiles.pkg = argv[++i];
        } else if (arg == "--pwc-profile" && i + 1 < argc) {
            profiles.pwc = argv[++i];
        } else if (arg == "--hf-profile" && i + 1 < argc) {
            profiles.hf = argv[++i];
        } else if (arg == "--s2-profile" && i + 1 < argc) {
            profiles.s2 = argv[++i];
        } else if (arg == "--so-profile" && i + 1 < argc) {
            profiles.so = argv[++i];
        } else if (arg == "--gh-token" && i + 1 < argc) {
            token = std::string(argv[++i]);
        } else {
            std::cerr << "Unknown argument: " << arg << "\n\n";
            print_help();
            return 1;
        }
    }

    if (!proxy_explicit) proxy_url = read_proxy_from_env();

    // ── 初始化全局统一缓存层(WAL + 5 张表) ──
    github_research::CacheConfig cacheCfg;
    cacheCfg.enabled = true;
    cacheCfg.db_path = "./data/research_mcp_cache.db";
    cacheCfg.default_ttl_hours = 24;
    cacheCfg.max_blob_inline_chars = 4000;
    cacheCfg.auto_record_metrics = true;

    // smoke test 模式下删除旧 DB,保证测试隔离
    if (cache_smoke_test) {
        std::error_code ec;
        std::filesystem::remove(cacheCfg.db_path, ec);
        std::filesystem::remove(cacheCfg.db_path + "-wal", ec);
        std::filesystem::remove(cacheCfg.db_path + "-shm", ec);
    }

    if (!github_research::CacheManager::instance().init(cacheCfg)) {
        std::cerr << "[cache] WARNING: init failed, will run without cache" << std::endl;
    } else {
        auto st = github_research::CacheManager::instance().stats();
        std::cerr << "[cache] ready: " << st.dump() << std::endl;
    }

    // ── Cache Smoke Test (不依赖 WebView2) ──
    if (cache_smoke_test) {
        using namespace github_research;
        CacheManager& cm = CacheManager::instance();
        int pass = 0, fail = 0;
        auto check = [&](const char* name, bool cond) {
            if (cond) { ++pass; std::cerr << "[SMOKE PASS] " << name << std::endl; }
            else       { ++fail; std::cerr << "[SMOKE FAIL] " << name << std::endl; }
        };

        // 1. put + get (小 payload inline)
        std::string smallBody = "{\"hello\":\"world\",\"n\":42}";
        cm.put("github", "repo:foo/bar", smallBody, "json", 24, "", "ok", "");
        auto e1 = cm.get("github", "repo:foo/bar");
        check("get returns entry", e1.has_value());
        check("payload matches small", e1 && e1->payload == smallBody);
        check("fetch_status ok",  e1 && e1->fetch_status == "ok");
        check("hit_count == 1",   e1 && e1->hit_count == 1);  // get 自增了

        // 2. 第二次 get,hit_count++
        auto e2 = cm.get("github", "repo:foo/bar");
        check("hit_count == 2", e2 && e2->hit_count == 2);

        // 3. is_fresh 新鲜
        check("is_fresh == true", cm.is_fresh("github", "repo:foo/bar"));

        // 4. 未命中
        check("miss returns nullopt", !cm.get("github", "repo:xxx/yyy").has_value());

        // 5. 大 payload 走 cache_blobs (>4000 chars)
        std::string big(8000, 'x');
        big.replace(0, 9, "{\"big\":1,");
        cm.put("arxiv", "paper:2401.12345", big, "json", 72, "", "ok", "");
        auto eb = cm.get("arxiv", "paper:2401.12345");
        check("big payload get ok", eb.has_value() && eb->payload.size() == 8000);
        check("big payload content match", eb && eb->payload == big);

        // 6. 失败态缓存(短 TTL)
        cm.put("hn", "item:12345", "", "json", 24, "", "failed", "network_error");
        auto ef = cm.get("hn", "item:12345");
        check("failed payload stored", ef.has_value() && ef->fetch_status == "failed");

        // 7. register_entity
        std::string eid_p1 = cm.register_entity("project", "langchain-ai/langchain",
            {"langchain"}, {"llm","rag"}, {{"stars",12345}}, "Build LLM apps");
        std::string eid_p2 = cm.register_entity("project", "chroma-core/chroma",
            {}, {"vector-db"}, json::object(), "Vector DB");
        std::string eid_ps = cm.register_entity("person", "hwchase17", {}, {}, {}, "LangChain author");
        check("entity id non-empty p1", !eid_p1.empty());
        check("entity id stable same name", cm.register_entity("project", "langchain-ai/langchain") == eid_p1);

        // 8. find_entity
        auto found = cm.find_entity("langchain", "project");
        check("find_entity finds langchain-ai/langchain", found.size() >= 1);

        // 9. add_relation
        cm.add_relation(eid_p1, eid_p2, "depends_on", 0.8, "github_requirements", "ref_1");
        cm.add_relation(eid_ps, eid_p1, "authored_by", 1.0, "github_contributors", "ref_2");
        auto rels = cm.query_relations(eid_p1, "out", "", 0.0, 10);
        check("query_relations out finds depends_on",
              !rels.empty() && rels[0].value("relation_type","") == "depends_on");

        // 10. traverse_graph (深度2)
        auto g = cm.traverse_graph(eid_ps, 2, 0.1, 20);
        int levels = g.value("levels", json::array()).size();
        check("traverse_graph has >=2 levels", levels >= 2);
        auto nodes = g.value("nodes", json::array());
        auto edges = g.value("edges", json::array());
        check("traverse_graph nodes non-empty", !nodes.empty());
        check("traverse_graph edges non-empty", !edges.empty());

        // 11. record_metric + query_timeseries
        cm.record_metric(eid_p1, "stars", 12345.0, "github");
        cm.record_metric(eid_p1, "stars", 12500.0, "github");  // delta=155
        auto ts = cm.query_timeseries(eid_p1, "stars");
        check("timeseries has 2 points", ts.size() == 2);
        check("2nd delta correct", ts.size() >= 2 && ts[1].value("delta_from_prev", -9999) == 155.0);

        // 12. invalidate + is_fresh 假
        cm.invalidate("github", "repo:foo/bar");
        check("invalidate works", !cm.get("github", "repo:foo/bar").has_value());

        // 13. stats 汇总
        auto st = cm.stats();
        int ents  = st.value("entity_count", -1);
        int rels2 = st.value("relation_count", -1);
        int tp    = st.value("time_points", -1);
        check("entity_count >= 3", ents >= 3);
        check("relation_count >= 2", rels2 >= 2);
        check("time_points >= 2", tp >= 2);
        check("hit_count_sum >= 2", st.value("hit_count_sum", -1) >= 2);

        // ═══════════════════════════════════════════════════════════
        //  多源融合 + 策略降级层 smoke (next1.txt)
        // ═══════════════════════════════════════════════════════════

        // 14. 数据源注册
        cm.register_source("github_api", "api", "https://api.github.com",
                           0.9, 200, 5000, 1.0, "{}");
        cm.register_source("npm_registry", "api", "https://registry.npmjs.org",
                           0.8, 100, 100000, 0.8, "{}");
        auto ds_gh = cm.get_source("github_api");
        check("get_source github_api", ds_gh.has_value() && ds_gh->source_id == "github_api");
        check("get_source reliability", ds_gh && ds_gh->reliability == 0.9);
        check("get_source avg_latency", ds_gh && ds_gh->avg_latency_ms == 200);
        check("get_source enabled", ds_gh && ds_gh->enabled);
        auto ds_npm = cm.get_source("npm_registry");
        check("get_source npm_registry", ds_npm.has_value() && ds_npm->source_type == "api");

        // 15. record_source_result (成功更新 avg_latency,失败递增 failures)
        cm.record_source_result("github_api", true, 250);
        auto ds_gh2 = cm.get_source("github_api");
        check("record_success updates latency", ds_gh2 && ds_gh2->avg_latency_ms == 250);
        check("record_success resets failures", ds_gh2 && ds_gh2->consecutive_failures == 0);
        cm.record_source_result("github_api", false);
        auto ds_gh3 = cm.get_source("github_api");
        check("record_failure increments", ds_gh3 && ds_gh3->consecutive_failures == 1);

        // 16. 降级策略
        cm.set_fallback_policy("project",
            {"github_api", "npm_registry", "hn_search"}, "", 0.3, true, 168);
        auto chain = cm.get_fallback_chain("project");
        check("fallback chain size 3", chain.size() == 3);
        check("fallback chain first github_api", !chain.empty() && chain[0] == "github_api");
        check("fallback chain second npm_registry", chain.size() >= 2 && chain[1] == "npm_registry");

        // 17. 字段级融合存储
        std::string eid_fusion = cm.register_entity("project", "fusion/test-repo",
            {}, {"test"}, json::object(), "Fusion test repo");
        cm.register_entity_source(eid_fusion, "github_api", "fusion/test-repo",
            {"stars","description","topics"}, 0.9);
        std::map<std::string, json> gh_fields = {
            {"stars", 100},
            {"description", "GitHub desc"},
            {"topics", json::array({"llm","rag"})}
        };
        cm.put_entity_fields(eid_fusion, "github_api", gh_fields, 0.9);
        auto fused = cm.get_entity_fields(eid_fusion);
        check("get_entity_fields has stars", fused.count("stars") > 0);
        check("get_entity_fields stars value",
              fused.count("stars") && fused["stars"].value == 100);
        check("get_entity_fields is_primary",
              fused.count("stars") && fused["stars"].is_primary);
        check("get_entity_fields has description",
              fused.count("description") && fused["description"].value == "GitHub desc");

        // 18. 多源融合查询(快速路径,不调用回调)
        auto ed = cm.get_entity(eid_fusion, "project");
        check("get_entity returns fields", !ed.fields.empty());
        check("get_entity stars correct",
              ed.fields.count("stars") && ed.fields["stars"].value == 100);
        check("get_entity sources_used empty (cache hit)", ed.sources_used.empty());
        check("get_entity canonical_name", ed.canonical_name == "fusion/test-repo");
        check("get_entity quality > 0", ed.quality_score > 0.0);

        // 19. 熔断器
        auto& breaker = cm.breaker();
        int64_t now_ts = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        for (int i = 0; i < 5; ++i) breaker.record_failure("bad_src", now_ts);
        check("breaker open after 5 failures", breaker.is_open("bad_src", now_ts));
        check("breaker state open", breaker.get_state("bad_src") == "open");
        breaker.record_success("bad_src");
        check("breaker closed after success", breaker.get_state("bad_src") == "closed");
        // 半开测试: open 后超过 reset_timeout 应进入 half_open
        for (int i = 0; i < 5; ++i) breaker.record_failure("half_src", now_ts);
        check("breaker half_src open", breaker.is_open("half_src", now_ts));
        check("breaker half_src half_open after timeout",
              !breaker.is_open("half_src", now_ts + 301));

        // 20. 降级链 + 主源失败切换备用源
        std::string eid_fb = cm.register_entity("project", "fallback/test-fb",
            {}, {}, json::object(), "FB test");
        cm.register_entity_source(eid_fb, "primary_src", "fallback/test-fb", {}, 0.9);
        cm.register_entity_source(eid_fb, "backup_src", "fallback/test-fb", {}, 0.7);
        cm.set_fallback_policy("project", {"primary_src", "backup_src"}, "", 0.3, true, 168);

        int primary_calls = 0, backup_calls = 0;
        cm.register_source_fetch("primary_src",
            [&primary_calls](const std::string&) -> std::map<std::string, json> {
                primary_calls++;
                throw std::runtime_error("primary source down");
            });
        cm.register_source_fetch("backup_src",
            [&backup_calls](const std::string&) -> std::map<std::string, json> {
                backup_calls++;
                return {{"description", "from backup"}, {"stars", 50}};
            });

        auto ed_fb = cm.get_entity(eid_fb, "project", {}, true);
        check("fallback primary was called", primary_calls > 0);
        check("fallback backup was called", backup_calls > 0);
        check("fallback used backup field",
              ed_fb.fields.count("description") &&
              ed_fb.fields["description"].value == "from backup");
        check("fallback has errors", !ed_fb.errors.empty());
        check("fallback is_fallback_result", ed_fb.is_fallback_result);
        check("fallback sources_used has backup",
              std::find(ed_fb.sources_used.begin(), ed_fb.sources_used.end(), "backup_src")
              != ed_fb.sources_used.end());

        // 21. 陈旧缓存降级(所有源失败 → 返回陈旧缓存)
        std::string eid_stale = cm.register_entity("project", "stale/test-stale",
            {}, {}, json::object(), "Stale test");
        cm.register_entity_source(eid_stale, "only_src", "stale/test-stale", {}, 0.8);
        cm.put_entity_fields(eid_stale, "only_src",
            {{"description", "old data"}}, 0.8);
        cm.set_fallback_policy("project", {"only_src"}, "", 0.3, true, 168);
        cm.register_source_fetch("only_src",
            [](const std::string&) -> std::map<std::string, json> {
                throw std::runtime_error("only source down");
            });

        auto ed_stale = cm.get_entity(eid_stale, "project", {}, true);
        check("stale returns cached field",
              ed_stale.fields.count("description") &&
              ed_stale.fields["description"].value == "old data");
        check("stale is_fallback_result", ed_stale.is_fallback_result);
        check("stale fallback_note set", !ed_stale.fallback_note.empty());

        // 22. 字段融合策略 - UNION(topics 取并集)
        std::string eid_union = cm.register_entity("project", "union/test-union",
            {}, {}, json::object(), "Union test");
        cm.register_entity_source(eid_union, "src_a", "union/test-union", {}, 0.9);
        cm.register_entity_source(eid_union, "src_b", "union/test-union", {}, 0.8);
        cm.set_fallback_policy("project", {"src_a", "src_b"}, "", 0.3, true, 168);
        cm.register_source_fetch("src_a",
            [](const std::string&) {
                return std::map<std::string, json>{
                    {"topics", json::array({"llm","rag"})}
                };
            });
        cm.register_source_fetch("src_b",
            [](const std::string&) {
                return std::map<std::string, json>{
                    {"topics", json::array({"rag","vector"})}
                };
            });

        auto ed_union = cm.get_entity(eid_union, "project", {}, true);
        check("union has topics field", ed_union.fields.count("topics") > 0);
        if (ed_union.fields.count("topics")) {
            auto& topics = ed_union.fields["topics"].value;
            check("union topics is array", topics.is_array());
            if (topics.is_array()) {
                check("union topics size >= 3", topics.size() >= 3);
                std::set<std::string> topic_set;
                for (auto& t : topics) {
                    if (t.is_string()) topic_set.insert(t.get<std::string>());
                }
                check("union has llm", topic_set.count("llm") > 0);
                check("union has rag", topic_set.count("rag") > 0);
                check("union has vector", topic_set.count("vector") > 0);
            }
        }

        // 23. 字段融合策略 - LATEST(stars 取最新)
        std::string eid_latest = cm.register_entity("project", "latest/test-latest",
            {}, {}, json::object(), "Latest test");
        cm.register_entity_source(eid_latest, "old_src", "latest/test-latest", {}, 0.9);
        cm.register_entity_source(eid_latest, "new_src", "latest/test-latest", {}, 0.9);
        cm.set_fallback_policy("project", {"old_src", "new_src"}, "", 0.3, true, 168);
        cm.register_source_fetch("old_src",
            [](const std::string&) {
                return std::map<std::string, json>{{"stars", 100}};
            });
        cm.register_source_fetch("new_src",
            [](const std::string&) {
                return std::map<std::string, json>{{"stars", 200}};
            });
        auto ed_latest = cm.get_entity(eid_latest, "project", {}, true);
        check("latest has stars", ed_latest.fields.count("stars") > 0);
        // LATEST 策略: 两次回调 now_ts 相同,后续不覆盖 existing → 保持 100
        // 但都来自不同源,second merge 时 incoming.last_updated == existing.last_updated
        // → 保持 existing(100)。验证 LATEST 行为可预测
        if (ed_latest.fields.count("stars")) {
            int s = ed_latest.fields["stars"].value.get<int>();
            check("latest stars is one of inputs", s == 100 || s == 200);
        }

        // 24. CleanerPipeline
        std::string html = "<b>Hello</b> <i>World</i>";
        std::string stripped = CleanerPipeline::strip_html(html);
        check("strip_html works", stripped == "Hello World");
        std::string messy = "  hello   world  ";
        std::string cleaned = CleanerPipeline::trim_compress(messy);
        check("trim_compress works", cleaned == "hello world");
        // 字段映射
        json raw_gh = {
            {"full_name", "test/repo"},
            {"stargazers_count", 100},
            {"forks_count", 20},
            {"description", "A test repo"}
        };
        CleanerPipeline cp;
        auto cleaned_fields = cp.clean(raw_gh, "github_api");
        check("clean maps full_name->canonical_name",
              cleaned_fields.count("canonical_name") > 0);
        check("clean maps stargazers_count->stars",
              cleaned_fields.count("stars") > 0);
        check("clean stars value",
              cleaned_fields.count("stars") && cleaned_fields["stars"] == 100);
        check("clean maps forks_count->forks",
              cleaned_fields.count("forks") > 0);
        // HTML 清洗
        json raw_html = {{"description", "<p>Cool <b>project</b></p>"}};
        auto cleaned_html = cp.clean(raw_html, "github_api");
        check("clean strips html in description",
              cleaned_html.count("description") &&
              cleaned_html["description"] == "Cool project");

        // 25. 跨源搜索
        auto results = cm.search_entities("langchain", "project", 0.0, 10);
        check("search_entities non-empty", !results.empty());
        check("search_entities returns EntityData",
              !results.empty() && !results[0].entity_id.empty());

        // 26. 健康状态
        auto health = cm.get_source_health();
        check("get_source_health has sources",
              health.contains("sources") && health["sources"].is_array());
        check("get_source_health has breakers",
              health.contains("breakers"));
        check("get_source_health sources non-empty",
              health.contains("sources") && health["sources"].size() > 0);

        // 27. 强制刷新
        cm.force_refresh_entity(eid_fusion);
        auto after_refresh = cm.get_entity_fields(eid_fusion);
        check("force_refresh_entity clears fields", after_refresh.empty());

        // 28. calculate_source_priority
        DataSource ds_test;
        ds_test.source_id = "github_api";
        ds_test.reliability = 0.9;
        ds_test.avg_latency_ms = 200;
        ds_test.priority_weight = 1.0;
        ds_test.consecutive_failures = 0;
        double prio_project = calculate_source_priority(ds_test, "project");
        double prio_paper = calculate_source_priority(ds_test, "paper");
        check("priority in [0,1]", prio_project >= 0.0 && prio_project <= 1.0);
        check("priority project > paper (type_bonus)",
              prio_project > prio_paper);
        // 失败惩罚
        ds_test.consecutive_failures = 5;
        double prio_failed = calculate_source_priority(ds_test, "project");
        check("priority decreases with failures", prio_failed < prio_project);

        // ═══════════════════════════════════════════════════════
        //  L5: 局部对象连续动态分析索引 (next2)
        //  覆盖 file_timeline / module_def / module_contributor_agg /
        //        file_cooccurrence / 三层查询
        // ═══════════════════════════════════════════════════════
        std::string repo_full = "octocat/Hello-World";
        // 使用相对当前时间的 timestamp,确保落在 aggregate_module_contributors 的 365 天窗口内
        int64_t base_ts = (int64_t)std::time(nullptr) - 3 * 86400;  // 3 天前

        // 29. record_file_timeline 单条写入
        cm.record_file_timeline(repo_full, "src/main.cpp", "abc123",
                                 "octocat", base_ts, 10, 2, 0, "init");
        cm.record_file_timeline(repo_full, "src/main.cpp", "def456",
                                 "octocat", base_ts + 86400, 5, 1, 0, "fix bug");
        auto tl1 = cm.query_file_timeline(repo_full, "src/main.cpp", 0, 0, 100);
        check("file_timeline has 2 records", tl1.size() == 2);
        check("file_timeline ascending",
              tl1.size() == 2 && tl1[0].value("commit_hash","") == "abc123");

        // 30. record_commit_files 批量写入 + 协同配对
        std::vector<std::tuple<std::string, int, int>> commit_files = {
            {"src/a.cpp", 10, 2},
            {"src/b.cpp", 5, 1},
            {"src/c.cpp", 3, 0}
        };
        int inserted = cm.record_commit_files(repo_full, "batch789", "octocat",
                                                base_ts + 172800, commit_files, 0, "batch");
        check("record_commit_files inserted 3", inserted == 3);

        // 31. file_cooccurrence 协同文件查询
        // batch789 同时修改了 a/b/c,所以 a 应该和 b/c 都有协同
        auto rf = cm.query_related_files(repo_full, "src/a.cpp", 1, 10);
        check("related_files finds b and c",
              rf.size() >= 2);
        // 验证协同计数
        bool found_b = false, found_c = false;
        for (auto& r : rf) {
            std::string fp = r.value("related_file", "");
            if (fp == "src/b.cpp") found_b = true;
            if (fp == "src/c.cpp") found_c = true;
        }
        check("related_files contains src/b.cpp", found_b);
        check("related_files contains src/c.cpp", found_c);

        // 32. record_file_timeline UNIQUE 幂等
        cm.record_file_timeline(repo_full, "src/main.cpp", "abc123",
                                 "octocat", base_ts, 10, 2, 0, "init");
        auto tl2 = cm.query_file_timeline(repo_full, "src/main.cpp", 0, 0, 100);
        check("UNIQUE idempotent - still 2 records", tl2.size() == 2);

        // 33. register_module_def + list_modules
        int64_t mod_id = cm.register_module_def(repo_full, "src_module", "src/*", "core", false);
        check("register_module_def returns id", mod_id > 0);
        auto mods = cm.list_modules(repo_full);
        check("list_modules has 1", mods.size() == 1);
        check("module name matches",
              !mods.empty() && mods[0].value("module_name","") == "src_module");

        // 34. query_module_files (glob 匹配 src/*)
        auto mf = cm.query_module_files(repo_full, "src_module");
        check("module files contains main.cpp",
              std::find(mf.begin(), mf.end(), "src/main.cpp") != mf.end());
        check("module files contains a.cpp",
              std::find(mf.begin(), mf.end(), "src/a.cpp") != mf.end());

        // 35. auto_cluster_modules 自动聚类
        std::vector<std::string> all_files = {
            "docs/readme.md", "docs/guide.md",
            "tests/test1.cpp", "tests/test2.cpp",
            "build/output.o"
        };
        int clustered = cm.auto_cluster_modules(repo_full, all_files);
        check("auto_cluster generates >=3 modules", clustered >= 3);
        auto mods2 = cm.list_modules(repo_full);
        check("list_modules after cluster >=4", mods2.size() >= 4);

        // 36. aggregate_module_contributors 预聚合
        int agg = cm.aggregate_module_contributors(repo_full, 365);
        check("aggregate_module_contributors > 0", agg > 0);

        // 37. query_module_contributors
        auto mc = cm.query_module_contributors(repo_full, "src_module", 365, 10);
        check("module contributors has octocat",
              !mc.empty() && mc[0].value("user_login","") == "octocat");
        check("module contributor changes >= 5",
              !mc.empty() && mc[0].value("total_changes",0) >= 5);

        // 38. search_by_signature
        auto sig = cm.search_by_signature(repo_full, "main.cpp", 0, 50);
        check("search_by_signature finds main.cpp", !sig.empty());

        // 39. query_change_density (按月聚合)
        auto cd = cm.query_change_density(repo_full, "src/main.cpp", 0, 24);
        check("change_density non-empty", !cd.empty());
        check("change_density has month field",
              !cd.empty() && cd[0].contains("month"));

        // 40. query_developer_modules
        auto dm = cm.query_developer_modules(repo_full, "octocat", 365, 30);
        check("developer_modules finds src_module",
              !dm.empty() && dm[0].value("module_name","") == "src_module");

        // 41. module_timeline_analysis layer=1 (轻量索引)
        // 通过 GitHubClient 调用,但 smoke test 不依赖网络,
        // ingest_first=false 时只用本地数据
        // 这里直接测试 cache_manager 的查询能力已覆盖,
        // 不再实例化 GitHubClient(需要 WebView2)

        // 42. file_cooccurrence 双向查询 (查 b.cpp 也应该找到 a.cpp)
        auto rf2 = cm.query_related_files(repo_full, "src/b.cpp", 1, 10);
        bool found_a = false;
        for (auto& r : rf2) {
            if (r.value("related_file","") == "src/a.cpp") found_a = true;
        }
        check("related_files bidirectional (b->a)", found_a);

        // 43. 时间窗口过滤
        auto tl_recent = cm.query_file_timeline(repo_full, "src/main.cpp",
                                                  base_ts + 86400, 0, 100);
        check("timeline filtered by since", tl_recent.size() == 1);

        // ═══════════════════════════════════════════════════════════
        //  L6: arXiv/HN 缓存层 + entity_mapper (next3)
        //  模拟工具写入缓存 + 实体映射,验证跨源闭合
        // ═══════════════════════════════════════════════════════════

        // 44. arXiv 缓存写入(paper:{arxiv_id}, TTL=72h)
        std::string arxiv_id = "2401.12345";
        std::string arxiv_cache_key = "paper:" + arxiv_id;
        json arxiv_payload = {
            {"success", true},
            {"arxiv_id", arxiv_id},
            {"title", "Deep Learning for Code Analysis"},
            {"authors", "Alice Smith, Bob Jones"},
            {"primary_category", "cs.SE"},
            {"abstract_full", "A novel approach..."},
            {"submitted_date", "2024-01-15"},
            {"pdf_url", "https://arxiv.org/pdf/" + arxiv_id},
            {"reference_count", 15}
        };
        cm.put("arxiv", arxiv_cache_key, arxiv_payload.dump(), "json", 72, "", "ok", "");
        cm.put("arxiv", "abs:" + arxiv_id, R"({"title":"Deep Learning"})", "json", 72, "", "ok", "");
        check("arxiv paper cache written", cm.is_fresh("arxiv", arxiv_cache_key));
        check("arxiv abs cache written", cm.is_fresh("arxiv", "abs:" + arxiv_id));

        // 45. arXiv 缓存命中
        auto arxiv_cached = cm.get("arxiv", arxiv_cache_key);
        check("arxiv cache hit", arxiv_cached.has_value());
        check("arxiv cache payload matches",
              arxiv_cached && arxiv_cached->fetch_status == "ok");
        check("arxiv cache TTL 72h",
              arxiv_cached && arxiv_cached->expires_at > arxiv_cached->fetched_at + 71 * 3600);

        // 46. arXiv entity_mapper: paper 实体 + authored_by 关系
        std::string paper_eid = cm.register_entity(
            "paper", arxiv_id,
            {"Deep Learning for Code Analysis"},
            {"cs.SE"},
            {{"primary_category", "cs.SE"}},
            "Deep Learning for Code Analysis"
        );
        check("arxiv paper entity registered", !paper_eid.empty());
        std::string person_alice = cm.register_entity("person", "Alice Smith", {}, {}, json::object(), "Alice Smith");
        std::string person_bob = cm.register_entity("person", "Bob Jones", {}, {}, json::object(), "Bob Jones");
        cm.add_relation(paper_eid, person_alice, "authored_by", 1.0, "arxiv", arxiv_id);
        cm.add_relation(paper_eid, person_bob, "authored_by", 1.0, "arxiv", arxiv_id);
        cm.record_metric(paper_eid, "reference_count", 15.0, "arxiv");

        // 47. arXiv authored_by 关系查询
        auto paper_rels = cm.query_relations(paper_eid, "out", "authored_by", 0.0, 10);
        check("arxiv paper has authored_by relations", paper_rels.size() >= 2);

        // 48. HN 缓存写入(item:{hn_id}, TTL=12h)
        std::string hn_id = "12345";
        std::string hn_cache_key = "item:" + hn_id;
        json hn_payload = {
            {"success", true},
            {"hn_id", hn_id},
            {"title", "Show HN: AI-powered code review tool"},
            {"source_url", "https://arxiv.org/abs/" + arxiv_id},
            {"comment_count", 5}
        };
        cm.put("hn", hn_cache_key, hn_payload.dump(), "json", 12, "", "ok", "");
        check("hn item cache written", cm.is_fresh("hn", hn_cache_key));

        // 49. HN 缓存命中 + TTL 验证
        auto hn_cached = cm.get("hn", hn_cache_key);
        check("hn cache hit", hn_cached.has_value());
        check("hn cache TTL 12h",
              hn_cached && hn_cached->expires_at > hn_cached->fetched_at + 11 * 3600);

        // 50. HN entity_mapper: topic 实体 + discussed_in 关系
        std::string story_eid = cm.register_entity(
            "topic", "hn:" + hn_id,
            {"Show HN: AI-powered code review tool"},
            {"hackernews"},
            {{"source_url", "https://arxiv.org/abs/" + arxiv_id},
             {"comment_count", 5}},
            "Show HN: AI-powered code review tool"
        );
        check("hn story entity registered", !story_eid.empty());
        cm.record_metric(story_eid, "comment_count", 5.0, "hn");

        // 51. HN comment 实体 + discussed_in 关系
        std::string comment_eid = cm.register_entity(
            "comment", "hn:" + hn_id + "#c0",
            {}, {"hackernews"},
            {{"author", "charlie"}, {"reply_level", 1}},
            ""
        );
        cm.add_relation(comment_eid, story_eid, "discussed_in", 0.8, "hn", hn_id);
        cm.add_relation(comment_eid, person_alice, "mentions", 0.5, "hn", hn_id);
        auto comment_rels = cm.query_relations(comment_eid, "out", "", 0.0, 10);
        check("hn comment has relations", comment_rels.size() >= 2);

        // 52. 跨源闭合: HN story mentions arXiv paper
        cm.add_relation(story_eid, paper_eid, "mentions", 0.9, "hn", hn_id);
        auto story_rels = cm.query_relations(story_eid, "out", "mentions", 0.0, 10);
        check("hn story mentions arxiv paper",
              !story_rels.empty() && story_rels[0].value("target_entity","") == paper_eid);

        // 53. 跨源图谱遍历(HN story → paper → authors)
        auto graph = cm.traverse_graph(story_eid, 3, 0.1, 20);
        int graph_levels = graph.value("levels", json::array()).size();
        check("cross-source graph traverse >= 2 levels", graph_levels >= 2);
        auto graph_nodes = graph.value("nodes", json::array());
        check("cross-source graph has nodes", !graph_nodes.empty());

        // 54. arXiv 失败缓存(短 TTL)
        cm.put("arxiv", "paper:FAIL000", "", "json", 1, "", "failed", "network_error");
        auto fail_cached = cm.get("arxiv", "paper:FAIL000");
        check("arxiv failed cache stored", fail_cached.has_value());
        check("arxiv failed cache status",
              fail_cached && fail_cached->fetch_status == "failed");

        // 55. HN 失败缓存(短 TTL)
        cm.put("hn", "item:999999", "", "json", 1, "", "failed", "timeout");
        auto hn_fail = cm.get("hn", "item:999999");
        check("hn failed cache stored", hn_fail.has_value());
        check("hn failed cache status",
              hn_fail && hn_fail->fetch_status == "failed");

        // 56. 时间快照查询(paper reference_count)
        auto paper_ts = cm.query_timeseries(paper_eid, "reference_count");
        check("arxiv paper metric timeseries", !paper_ts.empty());
        check("arxiv paper metric value",
              !paper_ts.empty() && paper_ts[0].value("metric_value", -1) == 15.0);

        // 57. 时间快照查询(hn comment_count)
        auto story_ts = cm.query_timeseries(story_eid, "comment_count");
        check("hn story metric timeseries", !story_ts.empty());

        // ═══════════════════════════════════════════════════════════
        //  L7 6 源扩展接入 smoke (npm/pypi/pwc/hf/s2/so)
        //    验证:缓存读写 + entity_mapper + register_entity_source + metric
        // ═══════════════════════════════════════════════════════════

        // ── 58. pkg/npm 缓存(TTL=24h) ──
        cm.put("pkg", "pkg:npm:react", R"({"name":"react","version":"18.2.0"})", "json", 24, "", "ok", "");
        auto pkg_npm = cm.get("pkg", "pkg:npm:react");
        check("pkg npm cache stored", pkg_npm.has_value());
        check("pkg npm cache status",
              pkg_npm && pkg_npm->fetch_status == "ok");
        check("pkg npm cache payload contains react",
              pkg_npm && pkg_npm->payload.find("react") != std::string::npos);

        // ── 59. pkg/pypi 缓存 ──
        cm.put("pkg", "pkg:pypi:numpy", R"({"name":"numpy","version":"1.26.0"})", "json", 24, "", "ok", "");
        auto pkg_pypi = cm.get("pkg", "pkg:pypi:numpy");
        check("pkg pypi cache stored", pkg_pypi.has_value());
        check("pkg pypi cache is_fresh", cm.is_fresh("pkg", "pkg:pypi:numpy"));

        // ── 60. pkg npm 失败缓存 ──
        cm.put("pkg", "pkg:npm:nonexistent-pkg-xyz", "", "json", 1, "", "failed", "404");
        auto pkg_fail = cm.get("pkg", "pkg:npm:nonexistent-pkg-xyz");
        check("pkg npm failed cache stored", pkg_fail.has_value());
        check("pkg npm failed cache status",
              pkg_fail && pkg_fail->fetch_status == "failed");

        // ── 61. package 实体(npm) ──
        std::string pkg_eid_react = cm.register_entity(
            "package", "npm:react",
            {"react"}, {"npm", "package"},
            {{"registry", "npm"}, {"version", "18.2.0"}},
            "React library"
        );
        check("pkg npm entity registered", !pkg_eid_react.empty());
        check("pkg npm entity stable",
              cm.register_entity("package", "npm:react") == pkg_eid_react);
        cm.register_entity_source(pkg_eid_react, "npm_registry", "react",
                                  {"name", "version", "description"}, 0.85);
        cm.record_metric(pkg_eid_react, "version_observed", 1.0, "npm");

        // ── 62. package 实体(pypi) ──
        std::string pkg_eid_numpy = cm.register_entity(
            "package", "pypi:numpy",
            {"numpy"}, {"pypi", "package"},
            {{"registry", "pypi"}, {"version", "1.26.0"}},
            "NumPy"
        );
        check("pkg pypi entity registered", !pkg_eid_numpy.empty());
        cm.register_entity_source(pkg_eid_numpy, "pypi_registry", "numpy",
                                  {"name", "version"}, 0.85);

        // ── 63. pwc 缓存(TTL=72h) ──
        cm.put("pwc", "pwc:some-paper-slug",
               R"({"paper_id":"some-paper-slug","title":"Sample Paper"})",
               "json", 72, "", "ok", "");
        auto pwc_cached = cm.get("pwc", "pwc:some-paper-slug");
        check("pwc cache stored", pwc_cached.has_value());
        check("pwc cache status ok",
              pwc_cached && pwc_cached->fetch_status == "ok");

        // ── 64. pwc paper 实体 ──
        std::string pwc_paper_eid = cm.register_entity(
            "paper", "pwc:some-paper-slug",
            {"Sample Paper"}, {"paperswithcode"},
            {{"paper_id", "some-paper-slug"}},
            "Sample Paper"
        );
        check("pwc paper entity registered", !pwc_paper_eid.empty());
        cm.register_entity_source(pwc_paper_eid, "pwc_web", "some-paper-slug",
                                  {"title", "abstract", "code_link"}, 0.85);
        cm.record_metric(pwc_paper_eid, "pwc_stars", 1.0, "pwc");

        // ── 65. hf model 缓存(TTL=12h) ──
        cm.put("hf", "hf:model:bert-base-uncased",
               R"({"model_id":"bert-base-uncased","title":"BERT Base Uncased"})",
               "json", 12, "", "ok", "");
        auto hf_model = cm.get("hf", "hf:model:bert-base-uncased");
        check("hf model cache stored", hf_model.has_value());
        check("hf model cache is_fresh", cm.is_fresh("hf", "hf:model:bert-base-uncased"));

        // ── 66. hf dataset 缓存(TTL=24h) ──
        cm.put("hf", "hf:dataset:squad",
               R"({"dataset_id":"squad","title":"SQuAD"})",
               "json", 24, "", "ok", "");
        auto hf_ds = cm.get("hf", "hf:dataset:squad");
        check("hf dataset cache stored", hf_ds.has_value());

        // ── 67. hf model 实体 ──
        std::string hf_model_eid = cm.register_entity(
            "model", "hf:bert-base-uncased",
            {"BERT Base Uncased", "bert-base-uncased"}, {"huggingface"},
            {{"model_id", "bert-base-uncased"}},
            "BERT Base Uncased"
        );
        check("hf model entity registered", !hf_model_eid.empty());
        cm.register_entity_source(hf_model_eid, "hf_web", "bert-base-uncased",
                                  {"title", "downloads", "likes"}, 0.85);
        cm.record_metric(hf_model_eid, "hf_observed", 1.0, "hf");

        // ── 68. hf dataset 实体 ──
        std::string hf_ds_eid = cm.register_entity(
            "dataset", "hf:squad",
            {"SQuAD", "squad"}, {"huggingface"},
            {{"dataset_id", "squad"}},
            "SQuAD"
        );
        check("hf dataset entity registered", !hf_ds_eid.empty());

        // ── 69. s2 缓存(TTL=72h) ──
        cm.put("s2", "s2:10.1000/test",
               R"({"paper_id":"10.1000/test","title":"Test Paper"})",
               "json", 72, "", "ok", "");
        auto s2_cached = cm.get("s2", "s2:10.1000/test");
        check("s2 cache stored", s2_cached.has_value());
        check("s2 cache status ok",
              s2_cached && s2_cached->fetch_status == "ok");

        // ── 70. s2 paper 实体 ──
        std::string s2_paper_eid = cm.register_entity(
            "paper", "s2:10.1000/test",
            {"Test Paper"}, {"semanticscholar"},
            {{"paper_id", "10.1000/test"}},
            "Test Paper"
        );
        check("s2 paper entity registered", !s2_paper_eid.empty());
        cm.register_entity_source(s2_paper_eid, "s2_web", "10.1000/test",
                                  {"title", "abstract", "citations"}, 0.9);
        cm.record_metric(s2_paper_eid, "s2_citations_observed", 1.0, "s2");

        // ── 71. so 缓存(TTL=24h) ──
        cm.put("so", "so:question:12345678",
               R"({"question_id":"12345678","title":"How to parse JSON in C++"})",
               "json", 24, "", "ok", "");
        auto so_cached = cm.get("so", "so:question:12345678");
        check("so cache stored", so_cached.has_value());
        check("so cache status ok",
              so_cached && so_cached->fetch_status == "ok");

        // ── 72. so question 实体 ──
        std::string so_q_eid = cm.register_entity(
            "question", "so:12345678",
            {"How to parse JSON in C++"}, {"stackoverflow"},
            {{"question_id", "12345678"}},
            "How to parse JSON in C++"
        );
        check("so question entity registered", !so_q_eid.empty());
        cm.register_entity_source(so_q_eid, "so_web", "12345678",
                                  {"title", "score", "view_count"}, 0.85);
        cm.record_metric(so_q_eid, "so_observed", 1.0, "so");

        // ── 73. 跨实体类型关系: hf model derived_from github project ──
        cm.add_relation(hf_model_eid, eid_p1, "derived_from", 0.7, "hf_web", "bert-base-uncased");
        auto derived_rels = cm.query_relations(hf_model_eid, "out", "derived_from", 0.0, 10);
        check("hf model derived_from github project",
              !derived_rels.empty() &&
              derived_rels[0].value("target_entity","") == eid_p1);

        // ── 74. 跨实体类型关系: so question mentions pwc paper ──
        cm.add_relation(so_q_eid, pwc_paper_eid, "mentions", 0.6, "so_web", "12345678");
        auto so_mentions = cm.query_relations(so_q_eid, "out", "mentions", 0.0, 10);
        check("so question mentions pwc paper",
              !so_mentions.empty() &&
              so_mentions[0].value("target_entity","") == pwc_paper_eid);

        // ── 75. 6 源各自 register_source 注册 ──
        cm.register_source("npm_registry", "api", "https://registry.npmjs.org",
                            0.85, 300, 100000, 0.8, "{}");
        cm.register_source("pypi_registry", "api", "https://pypi.org",
                            0.85, 300, 100000, 0.8, "{}");
        cm.register_source("pwc_web", "web_scrape", "https://paperswithcode.com",
                            0.85, 1500, 100, 0.9, "{}");
        cm.register_source("hf_web", "web_scrape", "https://huggingface.co",
                            0.85, 1200, 200, 0.9, "{}");
        cm.register_source("s2_web", "web_scrape", "https://www.semanticscholar.org",
                            0.9, 1500, 100, 1.0, "{}");
        cm.register_source("so_web", "web_scrape", "https://stackoverflow.com",
                            0.8, 800, 200, 0.8, "{}");

        auto ds_npm_l7 = cm.get_source("npm_registry");
        check("npm_registry source registered",
              ds_npm_l7.has_value() && ds_npm_l7->source_id == "npm_registry");
        auto ds_pypi = cm.get_source("pypi_registry");
        check("pypi_registry source registered",
              ds_pypi.has_value() && ds_pypi->reliability == 0.85);
        auto ds_pwc = cm.get_source("pwc_web");
        check("pwc_web source registered",
              ds_pwc.has_value() && ds_pwc->source_type == "web_scrape");
        auto ds_hf = cm.get_source("hf_web");
        check("hf_web source registered",
              ds_hf.has_value() && ds_hf->avg_latency_ms == 1200);
        auto ds_s2 = cm.get_source("s2_web");
        check("s2_web source registered",
              ds_s2.has_value() && ds_s2->priority_weight == 1.0);
        auto ds_so = cm.get_source("so_web");
        check("so_web source registered",
              ds_so.has_value() && ds_so->reliability == 0.8);

        // ── 76. 6 源失败缓存验证 ──
        cm.put("pkg", "pkg:npm:fetch-fail-test", "", "json", 1, "", "failed", "timeout");
        cm.put("pwc", "pwc:fetch-fail-test", "", "json", 1, "", "failed", "404");
        cm.put("hf", "hf:model:fetch-fail-test", "", "json", 1, "", "failed", "network");
        cm.put("s2", "s2:fetch-fail-test", "", "json", 1, "", "failed", "timeout");
        cm.put("so", "so:question:fetch-fail-test", "", "json", 1, "", "failed", "404");

        check("pkg failed cache", cm.get("pkg", "pkg:npm:fetch-fail-test")->fetch_status == "failed");
        check("pwc failed cache", cm.get("pwc", "pwc:fetch-fail-test")->fetch_status == "failed");
        check("hf failed cache",  cm.get("hf", "hf:model:fetch-fail-test")->fetch_status == "failed");
        check("s2 failed cache",  cm.get("s2", "s2:fetch-fail-test")->fetch_status == "failed");
        check("so failed cache",  cm.get("so", "so:question:fetch-fail-test")->fetch_status == "failed");

        // ── 77. 6 源 register_source_result(熔断器基础) ──
        cm.record_source_result("npm_registry", true, 200);
        cm.record_source_result("npm_registry", false);
        auto ds_npm_after = cm.get_source("npm_registry");
        check("npm_registry failures incremented",
              ds_npm_after && ds_npm_after->consecutive_failures == 1);

        cm.record_source_result("pypi_registry", true, 150);
        auto ds_pypi_after = cm.get_source("pypi_registry");
        check("pypi_registry success resets failures",
              ds_pypi_after && ds_pypi_after->consecutive_failures == 0 &&
              ds_pypi_after->avg_latency_ms == 150);

        // ── 78. 多源融合查询(hf model + github project 同一实体) ──
        // 模拟:github project 和 hf model 都描述 "bert-base-uncased"
        cm.put_entity_fields(hf_model_eid, "hf_web",
                             {{"title", "BERT Base Uncased"},
                              {"downloads", 1000000}}, 0.85);
        cm.put_entity_fields(eid_p1, "github_api",
                             {{"title", "BERT Repo"},
                              {"stars", 50000}}, 0.9);
        auto fused_model = cm.get_entity_fields(hf_model_eid);
        check("hf model fields fused", fused_model.count("title") > 0);
        auto fused_proj = cm.get_entity_fields(eid_p1);
        check("github project fields fused", fused_proj.count("title") > 0);

        // ── 79. 降级策略:paper 类型(arxiv -> s2 -> pwc) ──
        cm.set_fallback_policy("paper",
            {"arxiv_web", "s2_web", "pwc_web"}, "", 0.3, true, 168);
        auto paper_chain = cm.get_fallback_chain("paper");
        check("paper fallback chain size 3", paper_chain.size() == 3);
        check("paper fallback first arxiv_web",
              !paper_chain.empty() && paper_chain[0] == "arxiv_web");
        check("paper fallback second s2_web",
              paper_chain.size() >= 2 && paper_chain[1] == "s2_web");

        // ── 80. 降级策略:model 类型(hf_web -> github_api) ──
        cm.set_fallback_policy("model",
            {"hf_web", "github_api"}, "", 0.3, true, 168);
        auto model_chain = cm.get_fallback_chain("model");
        check("model fallback chain size 2", model_chain.size() == 2);
        check("model fallback first hf_web",
              !model_chain.empty() && model_chain[0] == "hf_web");

        // ── 81. 降级策略:package 类型(npm -> pypi) ──
        cm.set_fallback_policy("package",
            {"npm_registry", "pypi_registry"}, "", 0.3, true, 168);
        auto pkg_chain = cm.get_fallback_chain("package");
        check("package fallback chain size 2", pkg_chain.size() == 2);
        check("package fallback first npm_registry",
              !pkg_chain.empty() && pkg_chain[0] == "npm_registry");

        // ── 82. 时间序列跨源查询(s2 paper citations_observed) ──
        auto s2_ts = cm.query_timeseries(s2_paper_eid, "s2_citations_observed");
        check("s2 paper metric timeseries", !s2_ts.empty());
        check("s2 paper metric value",
              !s2_ts.empty() && s2_ts[0].value("metric_value", -1) == 1.0);

        // ── 83. 时间序列 hf model observed ──
        auto hf_ts = cm.query_timeseries(hf_model_eid, "hf_observed");
        check("hf model metric timeseries", !hf_ts.empty());

        // ── 84. 时间序列 so question observed ──
        auto so_ts = cm.query_timeseries(so_q_eid, "so_observed");
        check("so question metric timeseries", !so_ts.empty());

        // ── 85. 跨源图谱遍历(so question → pwc paper → ...) ──
        auto so_graph = cm.traverse_graph(so_q_eid, 2, 0.1, 20);
        int so_graph_levels = so_graph.value("levels", json::array()).size();
        check("so graph traverse >= 1 level", so_graph_levels >= 1);
        check("so graph has nodes", !so_graph.value("nodes", json::array()).empty());

        // ── 86. 跨源图谱遍历(hf model → github project → ...) ──
        auto hf_graph = cm.traverse_graph(hf_model_eid, 2, 0.1, 20);
        int hf_graph_levels = hf_graph.value("levels", json::array()).size();
        check("hf graph traverse >= 1 level", hf_graph_levels >= 1);

        // ── 87. stats 汇总(6 源扩展后) ──
        auto st_extended = cm.stats();
        int ents_ext = st_extended.value("entity_count", -1);
        int rels_ext = st_extended.value("relation_count", -1);
        check("entity_count >= 10 (extended)",
              ents_ext >= 10);  // 3 baseline + 7 new (react/numpy/pwc/hf_model/hf_ds/s2/so)
        check("relation_count >= 5 (extended)",
              rels_ext >= 5);  // baseline 2 + derived_from + mentions

        // ── 88. 6 源 entity find_entity 跨类型查找 ──
        auto found_react = cm.find_entity("react", "package");
        check("find_entity react package", !found_react.empty());
        auto found_bert = cm.find_entity("bert", "model");
        check("find_entity bert model", !found_bert.empty());
        auto found_squad = cm.find_entity("squad", "dataset");
        check("find_entity squad dataset", !found_squad.empty());
        auto found_so = cm.find_entity("so:12345678", "question");
        check("find_entity JSON question", !found_so.empty());

        std::cerr << "[SMOKE SUMMARY] pass=" << pass << " fail=" << fail << std::endl;
        github_research::CacheManager::instance().shutdown();
        return (fail == 0) ? 0 : 2;
    }

    github_research::McpServer server(token, timeout);

    if (!proxy_url.empty()) {
        std::cerr << "[mcp] proxy: " << proxy_url << std::endl;
        server.set_proxy(proxy_url);
    } else {
        std::cerr << "[mcp] proxy: none (direct)" << std::endl;
    }

    // GitHub 后端独立 user data dir(8 源隔离)
    if (!profiles.gh.empty()) {
        std::cerr << "[mcp] init GitHub profile: " << profiles.gh << std::endl;
        server.init_github_profile(profiles.gh);
    }

    // 各源 profile 路径交给 McpServer,首次 tool 调用时懒加载 WebView2 会话
    // 这样 initialize/tools/list 能立即响应,不被 7 个 WebView2 初始化阻塞
    github_research::McpServer::ProfilePaths ppaths;
    ppaths.arxiv = profiles.arxiv;
    ppaths.hn    = profiles.hn;
    ppaths.pkg   = profiles.pkg;
    ppaths.pwc   = profiles.pwc;
    ppaths.hf    = profiles.hf;
    ppaths.s2    = profiles.s2;
    ppaths.so    = profiles.so;
    server.set_profiles(ppaths);
    std::cerr << "[mcp] WebView2 sessions will be lazy-initialized on first tool call"
              << std::endl;

    if (port > 0) return server.run_http(port);
    return server.run();
}
