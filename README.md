# github-research-mcp (DeerFlow++)

8 源统一研究 MCP 服务,基于 **混合技术栈(libcurl + WebView2)** + **SQLite 统一缓存层** + **多源融合 + 熔断降级链** + **三层观测体系(L1 项目概览 / L2 单点深挖 / L3 关联图谱)** + **模块演进时序分析原语(子模块切片 / 维护链路归因)**。

## 特性

- **8 源 67 个工具**:GitHub / arXiv / Hacker News / npm+PyPI / Papers with Code / Hugging Face / Semantic Scholar / Stack Overflow
- **混合技术栈(各取所长)**:GitHub REST API 走 **libcurl**(轻量、无浏览器进程残留),7 个网页源走 **WebView2**(完整 Chromium 指纹、JS 渲染、DOM 提取)
- **后端可切换**:`GitHubClient` 通过 `std::unique_ptr<IHttpClient>` 多态持有 backend,构造时可选 `Backend::Curl`(默认) 或 `Backend::WebView2`
- **统一原始文本提取**:网页源统一返回 `{success, url, title, text, html}`,DOM 解析交给 AI
- **多实例会话隔离**:每个网页源独立 `WebViewSession` + 独立 user data dir,避免 Cookie / 缓存共享;**主源失败不影响备用源**
- **串行执行**:所有工具调用串行阻塞,无并行 / 线程池 / detach,简单可调试
- **MCP over stdio + HTTP**:JSON-RPC 2.0,兼容 Claude Desktop / llama.app / TRAE / Cursor
- **统一 SQLite 缓存层(WAL + 5 张表)**:cache_entries / cache_blobs / entities / relations / metrics
- **多源融合 + 字段级策略**:UNION / LATEST,自动按 source reliability 排序
- **熔断器 + 降级链**:主源失败自动切换备用源,所有源失败返回陈旧缓存(stale)
- **Entity Mapper + 关系图谱**:自动注册实体、建立跨源关系、记录时间快照
- **三层观测体系**:L1 项目概览(13 个 github_* 工具) / L2 单点深挖(局部对象连续动态分析索引) / L3 关联图谱(跨源 entity + relations)
- **模块演进时序分析原语**:`github_subdir_timeline_slice`(子模块拆分时序切片) + `github_maintenance_attribution`(维护链路归因),还原 Linux 内核等大仓库的维护流水线
- **跨源闭合**:HN story 自动检测 `source_url` 中的 arxiv.org,建立 `story -[mentions]-> paper` 跨源关系

## 架构

### 混合技术栈(libcurl + WebView2)+ 三层观测体系

```
┌────────────────────────────────────────────────────────────────┐
│              MCP Server (HTTP/stdio)                            │
└────────────────┬───────────────────────────────────────────────┘
                 │ JSON-RPC 2.0 dispatch
                 ▼
┌────────────────────────────────────────────────────────────────┐
│  dispatch_<source>_tool  (按工具名前缀路由)                      │
│  github_* (20) / arxiv_* (6) / hn_* (7) / pkg_* (4) /          │
│  pwc_*   (5) / hf_*   (7) / s2_*    (6) / so_*    (5)          │
│  + github_module_timeline_analysis (L2/L3)                      │
│  + github_subdir_timeline_slice    (原语A:子模块切片)            │
│  + github_maintenance_attribution  (原语B:维护链路归因)          │
│  + github_fetch_repo_detail / github_fetch_relation_network     │
│  + github_search_index / github_ingest_*                        │
└────────────────┬───────────────────────────────────────────────────────┘
                 │
   ┌─────────────┼─────────────┬─────────────┬─────────────┐
   ▼             ▼             ▼             ▼             ▼
┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐
│GitHub│ │arXiv │ │  HN  │ │ Pkg  │ │ PWC  │ │ ...  │
│Client│ │Session│ │Session│ │Session│ │Session│ │ ...
│libcurl│ │WebView2│ │WebView2│ │WebView2│ │WebView2│ │
│  +   │ │  +   │ │  +   │ │      │ │      │ │
│Cache │ │Cache │ │Cache │ │Cache │ │Cache │ │
│ +Entity +Entity +Entity                                    │
└──┬───┘ └──┬───┘ └──┬───┘ └──┬───┘ └──┬───┘ └──┬───┘
   │        │        │        │        │        │
   └────────┴────────┴────────┴────────┴────────┘
                         │
        ┌────────────────┴────────────────┐
        ▼                                 ▼
┌──────────────────┐         ┌──────────────────────────┐
│ libcurl 8.20     │         │  SQLite 统一缓存层         │
│ (GitHub API)     │         │  - cache_entries/blobs    │
│ + WebView2 基类   │         │  - entities / relations   │
│ (7 网页源)        │         │  - metrics (时间序列)      │
│ Navigate+ExecScript         │  + source_fusion           │
│                  │         │  + circuit breaker         │
│                  │         │  + fallback chain          │
└──────────────────┘         └──────────────────────────┘
                                       │
                                       ▼
                          ┌──────────────────────────┐
                          │  三层观测体系              │
                          │  L1: 项目概览 (github_*)  │
                          │  L2: 单点深挖 (timeline)   │
                          │  L3: 关联图谱 (跨源)       │
                          └──────────────────────────┘
```

### 调用模式(所有源统一)

1. `Navigate(url)` —— 导航到目标 URL
2. `WaitForNavigation(timeoutMs)` —— 等待 NavigationCompleted(pump 消息循环)
3. `ExecuteScript(kJsExtractRawPage)` —— 执行统一 JS 提取原始页面文本
4. 返回 `{success, url, title, text, html}` 给 MCP 客户端,AI 自行解析
5. **缓存层透明拦截**:成功/失败都写入 SQLite,下次同 URL 命中直接返回
6. **Entity Mapper 自动注册**:成功结果自动抽取实体字段、建立关系、记录时间快照

**统一 JS 脚本** `kJsExtractRawPage`(定义在 `webview_helpers.hpp`):

```javascript
(function(){
    var text = document.body ? document.body.innerText : "";
    if(text.length > 50000) text = text.substring(0, 50000);
    var html = document.documentElement.outerHTML;
    if(html.length > 50000) html = html.substring(0, 50000);
    return JSON.stringify({
        success: true,
        url: window.location.href,
        title: document.title || "",
        text: text,
        html: html
    });
})();
```

**设计理念**:工具只负责"取到页面内容",解析交给 AI。避免为每个站点维护复杂 DOM 选择器,降低维护成本。

**不使用** `fetch()` / `XMLHttpRequest`(WebView2 ExecuteScript 不 await Promise,同步 XHR 被 Chromium 限制)。

### 会话隔离(每个源独立 WebView2 + user data dir)

| 源 | 类 | user data dir 参数 | 数据源 ID |
|---|---|---|---|
| GitHub | `WebViewClient`(基于 `WebViewSession`) | `--gh-profile` | `github_api` |
| arXiv | `WebViewSession` | `--arxiv-profile` | `arxiv_web` |
| Hacker News | `WebViewSession` | `--hn-profile` | `hn_web` |
| npm/PyPI | `WebViewSession` | `--pkg-profile` | `pkg_web` |
| Papers with Code | `WebViewSession` | `--pwc-profile` | `pwc_web` |
| Hugging Face | `WebViewSession` | `--hf-profile` | `hf_web` |
| Semantic Scholar | `WebViewSession` | `--s2-profile` | `s2_web` |
| Stack Overflow | `WebViewSession` | `--so-profile` | `so_web` |

## 统一 SQLite 缓存层(WAL + 5 张表)

### 表结构

| 表 | 作用 |
|---|---|
| `cache_entries` | 缓存主表(source_type + cache_key 唯一,含 fetch_status / hit_count / TTL) |
| `cache_blobs` | 大 payload 存储(>4000 chars 自动外存) |
| `entities` | 实体索引(type + canonical_name,含 aliases / tags / attrs) |
| `relations` | 关系边(src_eid + dst_eid + relation_type + weight) |
| `metrics` | 时间序列(entity_id + metric_name + value + delta_from_prev) |
| `sources` | 数据源注册表(reliability / avg_latency / consecutive_failures / enabled) |
| `source_fusion` | 字段级融合存储(entity_id + source_id + field_name + value + weight) |
| `fallback_policies` | 降级策略(entity_type + fallback_chain + stale_ttl_hours) |

### 缓存读写流程

```cpp
// 读:命中"ok"且未过期 → 返回;否则继续抓取
auto cached = cm.get("arxiv", "paper:2401.01330");
if (cached && cached->fetch_status == "ok" && cm.is_fresh("arxiv", cache_key)) {
    return parse(cached->payload);  // cache_hit
}

// 写:成功(TTL=72h) / 失败(TTL=1h,避免重复回源)
cm.put("arxiv", "paper:2401.01330", payload, "json", 72, "", "ok", "");
cm.put("arxiv", "paper:2401.01330", "",        "json", 1,  "", "failed", "network_error");
```

### Entity Mapper + 关系图谱

每个源的成功结果自动抽取实体并建立关系:

| 源 | 实体类型 | 关系类型 | 时间快照 |
|---|---|---|---|
| GitHub | `project` / `person` | `depends_on` / `authored_by` | stars / commits |
| arXiv | `paper` | `authored_by` | citations / downloads |
| Hacker News | `topic` / `comment` | `discussed_in` / `mentions` | score / comment_count |
| npm/PyPI | `package` | `depends_on` / `authored_by` | weekly_downloads |
| Papers with Code | `paper` / `task` / `dataset` | `evaluated_on` / `proposes_method` | sota_score |
| Hugging Face | `model` / `dataset` | `derived_from` / `evaluated_on` | downloads / likes |
| Semantic Scholar | `paper` / `person` | `cites` / `authored_by` | citations / influential_count |
| Stack Overflow | `question` / `answer` / `tag` | `answered_by` / `tagged_with` | score / view_count |

### 跨源闭合(自动建立跨源关系)

HN 工具在抓取 story 时自动检测 `source_url`,若指向 arxiv.org 则提取 arxiv_id 并建立 `story -[mentions]-> paper` 关系:

```cpp
// hackernews_tools.cpp
if (sourceUrl.find("arxiv.org") != std::string::npos) {
    // 从 https://arxiv.org/abs/2510.01395 提取 arxiv_id = "2510.01395"
    std::string arxiv_id = extract_arxiv_id(sourceUrl);
    if (!arxiv_id.empty()) {
        std::string paper_eid = cm.register_entity("paper", arxiv_id, ...);
        cm.add_relation(story_eid, paper_eid, "mentions", 0.9, "hn", hnId);
    }
}
```

跨源图谱遍历示例:`hn_story → (mentions) → arxiv_paper → (authored_by) → person`

## 多源融合 + 熔断降级链

### 数据源注册 + source_fetch 回调

每个源在 `McpServer` 构造时注册数据源和 `source_fetch` 回调:

```cpp
// mcp_server.cpp
cm.register_source("github_api", "api", "https://api.github.com",
                    0.9 /* reliability */, 200 /* avg_latency_ms */,
                    5000 /* max_calls_per_hour */, 1.0 /* priority */, "{}");
cm.register_source_fetch("github_api",
    [client_ptr](const std::string& entity_key) -> std::map<std::string, json> {
        // entity_key = "owner/repo",通过 CleanerPipeline 提取标准化 fields
        json repo_info = client_ptr->get_repo_info(owner, repo);
        CleanerPipeline cleaner;
        return cleaner.clean(repo_info, "github_api");
    });
```

### 字段级融合(UNION / LATEST 策略)

```cpp
// 注册实体来源(支持多源同一实体)
cm.register_entity_source(eid, "github_api", "owner/repo",
    {"stars", "description", "topics"}, 0.9);

// 写入字段(自动按 source reliability 加权)
cm.put_entity_fields(eid, "github_api", {
    {"stars", 100}, {"description", "GitHub desc"}
}, 0.9);

// 读取融合后的字段(UNION 默认 / LATEST 取最新)
auto fields = cm.get_entity_fields(eid);
```

### 熔断器 + 降级链

```cpp
// 设置降级策略:主源 → 备用源1 → 备用源2 → 陈旧缓存
cm.set_fallback_policy("project",
    {"github_api", "npm_registry", "hn_search"},  // fallback chain
    "",                     // field_filter(空=全部)
    0.3,                    // min_reliability(低于此值不启用)
    true,                   // allow_stale(允许返回陈旧缓存)
    168                     // stale_ttl_hours(7天)
);

// 主源失败时自动切换:
// 1. record_source_result("github_api", false) 递增 consecutive_failures
// 2. 超过阈值 → 标记 enabled=false,熔断
// 3. get_fallback_chain("project") 返回下一个可用源
// 4. 所有源失败 → 返回陈旧缓存(若 allow_stale=true)
```

## 三层观测体系

### L1: 项目概览(13 个 github_* 工具)

提供仓库级、PR 级、issue 级的概览数据。对应原 49 个工具中的 github_* 系列。

### L2: 单点深挖(局部对象连续动态分析索引)

通过 `github_module_timeline_analysis` + `github_ingest_commit_timeline` + `github_ingest_recent_commits_timeline` 实现:

```
github_ingest_commit_timeline       → 拉取单个 commit 的文件变更
github_ingest_recent_commits_timeline → 批量拉取最近 N 天的 commit 时间线
github_module_timeline_analysis      → 三层职责统一分析入口:
  - target_type=file    → 单文件变更历史 + contributor_rank + change_density
  - target_type=module  → 模块级聚合(按目录前缀)
  - target_type=signature → 函数签名级(正则匹配)
  - layer=1/2/3         → 控制分析深度(related_files / contributor_rank / change_density)
```

### L3: 关联图谱(跨源 entity + relations)

通过 `github_fetch_relation_network` + `github_search_index` 实现:

```
github_search_index              → 实体检索(type / tags / aliases 模糊匹配)
github_fetch_repo_detail         → 仓库详情(融合多源字段)
github_fetch_relation_network    → 关系网络遍历(深度可配,默认 3 层)
```

跨源图谱示例(HN story 自动 mentions arXiv paper):

```
hn_story:49186720 ─[mentions]─→ arxiv_paper:2510.01395 ─[authored_by]─→ person:Myra Cheng
       │                              │
       └─[discussed_in]─→ hn_comment   └─[cited_by]─→ s2_paper:...
```

## 二进制程序下载(无需自行编译)

打 tag 推送后,GitHub Actions 云端自动编译并发布到 Release。直接下载即用,静态链接 MSVC CRT,纯净 Windows 10/11 x64 机器无需另装 VC++ 运行库。

1. 前往 [Releases](../../releases) 页面
2. 下载 `research-mcp-windows-x64.zip`
3. 解压后直接运行 `research-mcp.exe`(zip 内已附带 `WebView2Loader.dll` + `libcurl-x64.dll` + `curl-ca-bundle.crt`)

启动示例(解压目录下):

```powershell
.\research-mcp.exe --port 8765 --proxy http://127.0.0.1:7897
```

> 目标机器仍需自带 Edge Runtime(Win10/11 默认已有)。如需 8 源全量模式,参见下文「启动」章节的 `--xxx-profile` 参数。

## 环境要求(自行编译时)

- Windows 10/11 x64
- Edge Runtime(Win10/11 自带)
- Visual Studio 2022(C++ 桌面开发 + Windows 11 SDK)
- CMake 3.16+

## 依赖

| 依赖 | 获取方式 |
|---|---|
| WebView2 SDK | NuGet 包 `Microsoft.Web.WebView2`,解压到 `third_party/WebView2/` |
| nlohmann/json | vcpkg 安装,或单 header 放到 `third_party/json/include/` |
| SQLite | 源码已内置 `third_party/sqlite/sqlite3.c` |

## 构建

```powershell
cd D:\DeerFlow\DeerFlow++
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

构建产物:
- `build/Release/research-mcp.exe`
- `build/Release/WebView2Loader.dll`
- `build/tests/Release/test_smoke.exe`

## 启动

### 单源模式(仅 GitHub,向后兼容)

```powershell
.\research-mcp.exe --port 9876 --proxy http://127.0.0.1:7897
```

### arXiv + HN 双源模式(推荐最小化验证)

```powershell
.\research-mcp.exe --port 8765 `
  --arxiv-profile ./profiles/arxiv `
  --hn-profile    ./profiles/hn `
  --proxy http://127.0.0.1:7897
```

### 8 源全量模式(推荐)

```powershell
.\research-mcp.exe --port 8765 `
  --gh-profile    ./profiles/gh `
  --arxiv-profile ./profiles/arxiv `
  --hn-profile    ./profiles/hn `
  --pkg-profile   ./profiles/pkg `
  --pwc-profile   ./profiles/pwc `
  --hf-profile    ./profiles/hf `
  --s2-profile    ./profiles/s2 `
  --so-profile    ./profiles/so `
  --proxy http://127.0.0.1:7897
```

启动成功日志:

```
[cache] ready: {"blobs":1,"cache_entries":5,"entity_count":13,...}
[mcp] proxy: http://127.0.0.1:7897
[mcp] init arXiv session: ./profiles/arxiv
[session] WebView2 ready, profile: ./profiles/arxiv
[mcp] arXiv session ready
[mcp] arxiv source_fetch callback registered
[mcp] init HN session: ./profiles/hn
[session] WebView2 ready, profile: ./profiles/hn
[mcp] HackerNews session ready
[mcp] hn source_fetch callback registered
[mcp] server starting in HTTP mode on port 8765
[http] MCP server listening on http://127.0.0.1:8765/mcp (Ctrl+C to stop)
```

### 命令行参数

| 参数 | 说明 |
|---|---|
| `--port <PORT>` | HTTP MCP server 端口(默认:stdio 模式) |
| `--proxy <URL>` | 代理 URL(应用到所有 WebView 会话) |
| `--gh-profile <DIR>` | GitHub WebView user data dir(8 源隔离) |
| `--arxiv-profile <DIR>` | 启用 arXiv WebView 会话 |
| `--hn-profile <DIR>` | 启用 Hacker News WebView 会话 |
| `--pkg-profile <DIR>` | 启用 npm/PyPI WebView 会话 |
| `--pwc-profile <DIR>` | 启用 Papers with Code WebView 会话 |
| `--hf-profile <DIR>` | 启用 Hugging Face WebView 会话 |
| `--s2-profile <DIR>` | 启用 Semantic Scholar WebView 会话 |
| `--so-profile <DIR>` | 启用 Stack Overflow WebView 会话 |
| `--cache-smoke-test` | 运行 188 项缓存层烟雾测试(不依赖 WebView2) |
| `--help` / `-h` | 显示帮助 |

未指定 `--xxx-profile` 的源不启用,对应工具调用返回 `session not initialized`。

## 代理设置

WebView2 浏览器链路通过 Chromium 内核的 `--proxy-server` 命令行参数支持显式代理。
代理优先级:**命令行 `--proxy`** > 环境变量(`HTTPS_PROXY` > `HTTP_PROXY` > `ALL_PROXY`)。

### 方式 1:命令行参数(推荐)

```powershell
.\research-mcp.exe --port 8765 --proxy http://127.0.0.1:7897
```

### 方式 2:环境变量

```powershell
$env:HTTPS_PROXY = "http://127.0.0.1:7897"
$env:HTTP_PROXY  = "http://127.0.0.1:7897"
.\research-mcp.exe --port 8765
```

### 代理参数格式

| 格式 | 示例 | 说明 |
|---|---|---|
| `http://host:port` | `http://127.0.0.1:7897` | HTTP 代理(自动剥离 `http://` 前缀) |
| `http://user:pass@host:port` | `http://user:pass@proxy.example.com:8080` | 带认证的代理 |
| `socks5://host:port` | `socks5://127.0.0.1:1080` | SOCKS5 代理 |

## 验证调用

### tools/list

```powershell
$body = '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
$r = Invoke-RestMethod -Uri http://127.0.0.1:8765/mcp -Method Post -ContentType 'application/json' -Body $body
"tools count: $($r.result.tools.Count)"
# → tools count: 65
```

### arXiv 论文详情(支持 cache_hit)

```powershell
$body = '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"arxiv_fetch_paper_detail","arguments":{"arxiv_id":"2401.01330","fetch_full_text":false}}}'
$r = Invoke-RestMethod -Uri http://127.0.0.1:8765/mcp -Method Post -ContentType 'application/json' -Body $body -TimeoutSec 60
$r.result.content[0].text
```

首次调用返回(无 `cache_hit` 字段):

```json
{
  "arxiv_id": "2401.01330",
  "title": "TREC iKAT 2023: The Interactive Knowledge Assistance Track Overview",
  "authors": "Mohammad Aliannejadi, ...",
  "abstract_full": "...",
  "primary_category": "Information Retrieval (cs.IR)",
  "pdf_url": "https://arxiv.org/pdf/2401.01330",
  "submitted_date": "[Submitted on 2 Jan 2024 ...]",
  "success": true
}
```

二次调用返回(命中缓存,`cache_hit=true`,`cache_expires_at` 给出过期时间戳):

```json
{
  "arxiv_id": "2401.01330",
  "title": "...",
  "cache_hit": true,
  "cache_expires_at": 1786248093,
  "success": true
}
```

### Hacker News story 详情(自动建立跨源 mentions 关系)

```powershell
# 找一个 source_url 指向 arxiv.org 的 story
$body = '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"hn_fetch_detailed_story","arguments":{"hn_id":"49186720","fetch_comments":false,"fetch_external_article":false}}}'
$r = Invoke-RestMethod -Uri http://127.0.0.1:8765/mcp -Method Post -ContentType 'application/json' -Body $body -TimeoutSec 60
$r.result.content[0].text
```

返回(注意 `source_url`):

```json
{
  "hn_id": "49186720",
  "title": "Sycophantic AI Decreases Prosocial Intentions and Promotes Dependence (2025)",
  "source_url": "https://arxiv.org/abs/2510.01395",
  "article_fetch_status": "disabled",
  "comment_count": 0,
  "success": true
}
```

调用后 `entity_mapper` 自动:
1. 注册 `topic:49186720` 实体
2. 从 `source_url` 提取 `arxiv_id=2510.01395`
3. 注册 `paper:2510.01395` 实体
4. 建立 `topic ─[mentions, weight=0.9]→ paper` 跨源关系

### GitHub 模块时间线分析(L2 单点深挖)

```powershell
$body = '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"github_module_timeline_analysis","arguments":{"owner":"bytedance","repo":"deer-flow","target_type":"file","target_path":"src/graph/builder.py","time_range":"90d","layer":2,"ingest_first":true}}}'
$r = Invoke-RestMethod -Uri http://127.0.0.1:8765/mcp -Method Post -ContentType 'application/json' -Body $body -TimeoutSec 240
$r.result.content[0].text
```

返回:

```json
{
  "repo_full_name": "bytedance/deer-flow",
  "target_type": "file",
  "target_path": "src/graph/builder.py",
  "time_range": "90d",
  "layer": 2,
  "timeline": [
    {"sha": "...", "date": "2024-12-15", "author": "...", "message": "...", "files_changed": [...]}
  ],
  "timeline_count": 12,
  "contributor_rank": [{"author": "...", "commits": 5, "files_changed": 23}],
  "change_density": [{"month": "2024-12", "lines_added": 156, "lines_deleted": 42}],
  "related_files": [{"path": "...", "co_change_count": 8}]
}
```

### GitHub 仓库信息(主源失败时熔断 + 失败缓存)

```powershell
$body = '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"github_get_repo_info","arguments":{"owner":"bytedance","repo":"deer-flow"}}}'
$r = Invoke-RestMethod -Uri http://127.0.0.1:8765/mcp -Method Post -ContentType 'application/json' -Body $body -TimeoutSec 120
"isError: $($r.result.isError)"
$r.result.content[0].text
```

主源 WebView2 初始化失败时:

```json
{
  "error": "WebView2 backend initialization failed (no fallback, single tech stack)",
  "status_code": 0,
  "url": "https://api.github.com/repos/bytedance/deer-flow"
}
```

失败结果写入缓存(`fetch_status=failed`,TTL=1h),短时间内重复调用直接返回失败缓存,不再重复尝试 WebView2 初始化。

## 工具列表(65 个)

### GitHub(18 个)

| 工具 | 说明 | 层 |
|---|---|---|
| `github_get_repo_info` | 仓库基础信息 | L1 |
| `github_get_readme` | README 全文(markdown) | L1 |
| `github_get_tree` | 目录树(格式化文本) | L1 |
| `github_get_languages` | 语言分布 | L1 |
| `github_get_contributors` | 贡献者列表 | L1 |
| `github_get_commits` | 近期提交(支持 `branch` / `sha` 参数查询非默认分支) | L1 |
| `github_get_branches` | 全部分支列表(用于按分支汇总提交时间线) | L1 |
| `github_get_issues` | Issues 列表 | L1 |
| `github_get_pull_requests` | PR 列表 | L1 |
| `github_get_releases` | 发布历史 | L1 |
| `github_summarize_repo` | 综合摘要 | L1 |
| `github_search_repositories` | 按项目名 / 语言 / topic / stars 搜索仓库(trending / discovery) | L1 |
| `github_search_users` | 按作者名 / 组织 / 地区 / 粉丝数搜索用户 | L1 |
| `github_search_index` | 实体检索(type / tags / aliases 模糊匹配) | L3 |
| `github_fetch_repo_detail` | 仓库详情(融合多源字段) | L3 |
| `github_fetch_relation_network` | 关系网络遍历(深度可配) | L3 |
| `github_ingest_commit_timeline` | 拉取单个 commit 的文件变更 | L2 |
| `github_ingest_recent_commits_timeline` | 批量拉取最近 N 天的 commit 时间线 | L2 |
| `github_module_timeline_analysis` | 三层职责统一分析入口(file/module/signature) | L2/L3 |

### arXiv(6 个)

| 工具 | 说明 |
|---|---|
| `arxiv_search_papers` | 按关键词 / 分类搜索论文 |
| `arxiv_get_paper_detail` | 获取论文详情(标题、作者、摘要、PDF 链接) |
| `arxiv_get_pdf_link` | 根据 arXiv ID 生成 PDF / abs 链接(零网络) |
| `arxiv_check_available` | 检查 arXiv 网站可用性 |
| `arxiv_search_index` | 论文实体检索 |
| `arxiv_fetch_paper_detail` | 论文详情 + cache_hit 标记 + entity_mapper 写入 |

### Hacker News(7 个)

| 工具 | 说明 |
|---|---|
| `hn_get_top_stories` | 头条故事 |
| `hn_get_new_stories` | 最新故事 |
| `hn_get_best_stories` | 精选故事 |
| `hn_get_item` | 获取单个 item(故事 / 评论) |
| `hn_search_by_keyword` | 按关键词搜索 |
| `hn_get_latest_index` | 最新索引快照 |
| `hn_fetch_detailed_story` | 故事详情 + 跨源 mentions 关系自动建立 |

### npm / PyPI(5 个)

| 工具 | 说明 |
|---|---|
| `pkg_search_npm` | 搜索 npm 包 |
| `pkg_get_npm_detail` | 获取 npm 包详情 |
| `pkg_search_pypi` | 搜索 PyPI 包 |
| `pkg_get_pypi_detail` | 获取 PyPI 包详情 |
| `pkg_fetch_detail` | 包详情 + 缓存(TTL=24h, cache_hit) + package 实体注册 |

### Papers with Code(6 个)

| 工具 | 说明 |
|---|---|
| `pwc_search_papers` | 搜索论文 |
| `pwc_get_paper_detail` | 论文详情 |
| `pwc_get_sota` | 获取 SOTA(State-of-the-Art)结果 |
| `pwc_search_tasks` | 搜索任务 |
| `pwc_search_datasets` | 搜索数据集 |
| `pwc_fetch_paper_detail` | 论文详情 + 缓存(TTL=72h) + paper 实体注册 + pwc_stars 时间快照 |

### Hugging Face(9 个)

| 工具 | 说明 |
|---|---|
| `hf_search_models` | 搜索模型 |
| `hf_get_model_info` | 模型详情 |
| `hf_get_model_readme` | 模型 README |
| `hf_search_datasets` | 搜索数据集 |
| `hf_get_dataset_info` | 数据集详情 |
| `hf_get_trending_models` | 热门模型 |
| `hf_search_spaces` | 搜索 Spaces |
| `hf_fetch_model_detail` | 模型详情 + 缓存(TTL=12h) + model 实体注册 + derived_from 关系 |
| `hf_fetch_dataset_detail` | 数据集详情 + 缓存(TTL=24h) + dataset 实体注册 |

### Semantic Scholar(7 个)

| 工具 | 说明 |
|---|---|
| `s2_search_papers` | 搜索论文 |
| `s2_get_paper_detail` | 论文详情 |
| `s2_get_citations` | 引用列表 |
| `s2_get_references` | 参考文献列表 |
| `s2_get_author_papers` | 作者论文列表 |
| `s2_search_author` | 搜索作者 |
| `s2_fetch_paper_detail` | 论文详情 + 缓存(TTL=72h) + paper 实体注册 + citations_observed 时间快照 |

### Stack Overflow(6 个)

| 工具 | 说明 |
|---|---|
| `so_search_questions` | 搜索问题 |
| `so_get_question_detail` | 问题详情(含答案) |
| `so_get_top_answers` | 获取热门答案 |
| `so_search_by_tags` | 按标签搜索 |
| `so_get_similar` | 获取相似问题 |
| `so_fetch_question_detail` | 问题详情 + 缓存(TTL=24h) + question 实体注册 + mentions 关系 + so_observed 时间快照 |

## 客户端配置

### Claude Desktop / TRAE

```json
{
  "mcpServers": {
    "github-research": {
      "command": "D:\\DeerFlow\\DeerFlow++\\build\\Release\\research-mcp.exe",
      "args": [
        "--port", "8765",
        "--gh-profile",    "D:\\DeerFlow\\DeerFlow++\\build\\Release\\profiles\\gh",
        "--arxiv-profile", "D:\\DeerFlow\\DeerFlow++\\build\\Release\\profiles\\arxiv",
        "--hn-profile",    "D:\\DeerFlow\\DeerFlow++\\build\\Release\\profiles\\hn",
        "--pkg-profile",   "D:\\DeerFlow\\DeerFlow++\\build\\Release\\profiles\\pkg",
        "--pwc-profile",   "D:\\DeerFlow\\DeerFlow++\\build\\Release\\profiles\\pwc",
        "--hf-profile",    "D:\\DeerFlow\\DeerFlow++\\build\\Release\\profiles\\hf",
        "--s2-profile",    "D:\\DeerFlow\\DeerFlow++\\build\\Release\\profiles\\s2",
        "--so-profile",    "D:\\DeerFlow\\DeerFlow++\\build\\Release\\profiles\\so",
        "--proxy", "http://127.0.0.1:7897"
      ],
      "env": {
        "GITHUB_TOKEN": "ghp_xxx"
      }
    }
  }
}
```

### llama.cpp 集成

```powershell
llama-server.exe ^
  -m Qwen2.5-7B-Instruct.Q4_K_M.gguf ^
  --port 8080 ^
  --mcp http://127.0.0.1:8765/mcp
```

挂载后 llama.cpp 自动执行 `initialize` 握手 → `tools/list`,把 59 个工具注册为 `McpServer` tool 的子项,LLM 可通过 `McpServer(name="arxiv_search_papers", arguments={...})` 形式调用。

## 协议兼容性

| 协议项 | 支持情况 | 说明 |
|---|---|---|
| JSON-RPC 2.0 | ✅ | 标准 request/response/notification |
| MCP 版本 | 2024-11-05 | `initialize` 协议握手版本 |
| 传输方式 | stdio + HTTP | HTTP 路径 `/mcp`,Content-Type `application/json` |
| 批量请求 | ✅ | JSON 数组形式的批量 JSON-RPC |
| CORS | ✅ | 响应头 `Access-Control-Allow-Origin: *` |
| OPTIONS 预检 | ✅ | 自动返回 200 |
| `tools/list` | ✅ | 59 个工具(8 源) |
| `tools/call` | ✅ | 支持 `isError` 字段标记失败 |
| `ping` | ✅ | 心跳保活 |
| `shutdown` | ✅ | 触发 server 优雅停止 |

## 错误处理

所有错误返回 `isError=true`,content 为 JSON 字符串:

```json
{"error":"repository not found","status_code":404,"url":"https://api.github.com/repos/..."}
```

GitHub 限流错误附带 `reset_at`:

```json
{"error":"rate limit exceeded","status_code":429,"reset_at":"1785724800"}
```

会话未初始化错误:

```json
{"error":"ERROR: arXiv WebView session not initialized."}
```

WebView2 后端初始化失败(无 fallback):

```json
{"error":"WebView2 backend initialization failed (no fallback, single tech stack)","status_code":0,"url":"..."}
```

## 故障排查

### `WebView2 initialization timeout`

Edge Runtime 缺失或被沙箱阻止。检查:
1. `reg query "HKLM\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}" /v pv` 应返回版本号
2. 在非沙箱环境(真实 Windows 终端)运行
3. 设置 `WEBVIEW2_USER_DATA_DIR` 环境变量到可写目录
4. 显式指定 `--gh-profile` / `--arxiv-profile` 等参数,避免使用默认路径

### `HTTP 403`(GitHub)

GitHub API 限流(每小时 60 次/未认证)。解决:
1. 设置 `GITHUB_TOKEN` 环境变量(Personal Access Token,每小时 5000 次)
2. 等待限流重置(查看响应头 `X-RateLimit-Reset`)

### `session not initialized`

对应源的 `--xxx-profile` 参数未指定。检查启动命令是否包含全部 8 个 `--xxx-profile` 参数。

### `fetch_result preview: {}`

WebView2 ExecuteScript 不 await Promise 的已知问题。本服务已改用 `Navigate + ExecuteScript` 模式(读取 `document.body.innerText`),不再使用 `fetch()` / `XMLHttpRequest`。如仍出现此错误,请确认使用最新构建的 exe。

### 主源失败但备用源正常

**这是设计预期行为**,不是 bug:
- 每个 WebView 会话独立 user data dir,主源(GitHub)失败不影响 arXiv / HN 等备用源
- 失败缓存写入 SQLite(TTL=1h),短时间内重复调用直接返回失败缓存,不再重复初始化 WebView2
- 调用 `set_fallback_policy` 配置降级链后,主源失败自动切换备用源

## 测试

### 缓存层烟雾测试(188 项,不依赖 WebView2)

```powershell
.\build\Release\research-mcp.exe --cache-smoke-test
```

测试覆盖:

| 层 | 用例数 | 覆盖范围 |
|---|---:|---|
| L0 基础缓存 | 13 | put/get/is_fresh/invalidate/hit_count/big_payload/failed_status |
| L1 多源融合 | 14 | register_source/get_source/record_source_result/fallback_chain |
| L2 熔断+降级 | 14 | consecutive_failures/enable_disable/stale_cache/dynamic_priority |
| L3 字段融合策略 | 18 | UNION/LATEST/reliability_weight/register_entity_source/put_entity_fields |
| L4 清洗+搜索 | 22 | CleanerPipeline/cross_source_search/force_refresh |
| L5 实体+关系+时间序列 | 22 | register_entity/find_entity/add_relation/traverse_graph/record_metric |
| L6 arXiv/HN 真实接入 | 24 | paper cache(72h)/abs cache/story cache(12h)/authored_by/discussed_in/mentions 跨源闭合/失败缓存/时间序列 |
| L7 6 源扩展接入 | 61 | npm/pypi(8)+pwc(6)+hf(12)+s2(8)+so(8): 缓存读写/实体注册/跨源关系/降级策略/时间序列/数据源注册/熔断器/失败缓存 |

预期输出:

```
[SMOKE PASS] get returns entry
[SMOKE PASS] payload matches small
...
[SMOKE PASS] cross-source graph traverse >= 2 levels
[SMOKE PASS] cross-source graph has nodes
...
[SMOKE PASS] entity_count >= 10 (extended)
[SMOKE PASS] relation_count >= 5 (extended)
[SMOKE PASS] find_entity react package
[SMOKE PASS] find_entity bert model
[SMOKE PASS] find_entity squad dataset
[SMOKE PASS] find_entity JSON question
[SMOKE SUMMARY] pass=188 fail=0
```

### 单元测试

```powershell
cd D:\DeerFlow\DeerFlow++
cmake --build build --config Release --target test_smoke
.\build\tests\Release\test_smoke.exe
```

### 真实网络端到端验证(需启动 Server)

参见上文「验证调用」章节,关键验证点:

| 验证项 | 期望结果 |
|---|---|
| `tools/list` 返回工具数 | 59 |
| `arxiv_fetch_paper_detail` 首次调用 | 返回论文详情(无 `cache_hit` 字段) |
| `arxiv_fetch_paper_detail` 二次调用 | `cache_hit=true` + `cache_expires_at` 时间戳 |
| `hn_fetch_detailed_story` 返回 `source_url=arxiv.org` | 自动建立 `story -[mentions]-> paper` 跨源关系 |
| `github_module_timeline_analysis` | 返回 `timeline_count > 0`(若仓库有近期提交) |
| 主源 WebView2 失败时调用备用源 | 备用源(arXiv/HN)正常返回,主源错误隔离 |

## 与 DeerFlow 原版的差异

| 维度 | DeerFlow 原版 | 本地 MCP 版 |
|---|---|---|
| 实现语言 | Python skill + agent runtime | C++17 可执行文件 + LLM 客户端 |
| HTTP 后端 | Python requests | WebView2(Chromium 内核) |
| 浏览器指纹 | 无 | 完整(与 Edge 一致) |
| 反爬能力 | 弱 | 强(真实浏览器) |
| 数据源 | GitHub 单源 | 8 源统一接入 |
| 缓存层 | 无 | SQLite WAL + 5 张表 + 188 项烟雾测试 |
| 多源融合 | 无 | 字段级 UNION/LATEST 策略 + 熔断器 + 降级链 |
| 关系图谱 | 无 | Entity Mapper + 跨源 mentions 自动建立 |
| 三层观测 | 无 | L1 概览 / L2 单点深挖 / L3 关联图谱 |
| 编排主体 | agent runtime | 本地 LLM 客户端 |
| 平台 | 跨平台 | Windows 10/11 专属 |

## llama.cpp server 语义驱动 MCP 功能列表

### 设计原理

使用 `<尖括号>` 语义模板驱动 llama.cpp server 将自然语言用户请求解析为具体 MCP 工具调用。
llama.cpp 通过 `--mcp http://host:port/mcp` 挂载后,LLM 可直接识别语义模板并生成对应参数。

### GitHub 源(19 个模板)

| 语义模板 | 对应 MCP 工具 |
|---|---|
| `<获取 github 项目 <owner/repo> 的基本信息>` | `github_get_repo_info` |
| `<读取 github 项目 <owner/repo> 的 README 文档>` | `github_get_readme` |
| `<列出 github 项目 <owner/repo> 的目录树,深度 <depth>>` | `github_get_tree` |
| `<统计 github 项目 <owner/repo> 的语言分布>` | `github_get_languages` |
| `<获取 github 项目 <owner/repo> 的贡献者列表,前 <N> 名>` | `github_get_contributors` |
| `<分析 github 项目 <owner/repo> 在 <分支> 的最近 <N> 条提交>` | `github_get_commits` |
| `<枚举 github 项目 <owner/repo> 的所有分支>` | `github_get_branches` |
| `<查询 github 项目 <owner/repo> 的 issues,状态 <state>,前 <N> 条>` | `github_get_issues` |
| `<查询 github 项目 <owner/repo> 的 PR,状态 <state>,前 <N> 条>` | `github_get_pull_requests` |
| `<获取 github 项目 <owner/repo> 的发布历史,前 <N> 个版本>` | `github_get_releases` |
| `<汇总 github 项目 <owner/repo> 的整体概览>` | `github_summarize_repo` |
| `<搜索 github 上 <关键词/语言/topic/星数> 的热点项目>` | `github_search_repositories` |
| `<搜索 github 上 <关键词> 的用户或组织>` | `github_search_users` |
| `<在本地缓存索引中搜索 github 项目 <关键词>>` | `github_search_index` |
| `<抓取 github 项目 <owner/repo> 的详情并写入缓存+实体注册>` | `github_fetch_repo_detail` |
| `<查询 github 项目 <owner/repo> 的关联实体网络>` | `github_fetch_relation_network` |
| `<导入 github 项目 <owner/repo> 的完整提交时间线>` | `github_ingest_commit_timeline` |
| `<导入 github 项目 <owner/repo> 的近期提交时间线>` | `github_ingest_recent_commits_timeline` |
| `<分析 github 项目 <owner/repo> 的模块演进时间线>` | `github_module_timeline_analysis` |

### arXiv 源(6 个模板)

| 语义模板 | 对应 MCP 工具 |
|---|---|
| `<搜索 arxiv 上 <关键词> 的论文,前 <N> 篇>` | `arxiv_search_papers` |
| `<获取 arxiv 论文 <arxiv_id> 的详细信息>` | `arxiv_get_paper_detail` |
| `<获取 arxiv 论文 <arxiv_id> 的 PDF 下载链接>` | `arxiv_get_pdf_link` |
| `<检查 arxiv 论文 <arxiv_id> 是否可访问>` | `arxiv_check_available` |
| `<在本地缓存索引中搜索 arxiv 论文 <关键词>>` | `arxiv_search_index` |
| `<抓取 arxiv 论文 <arxiv_id> 的详情并写入缓存+实体注册>` | `arxiv_fetch_paper_detail` |

### Hacker News 源(7 个模板)

| 语义模板 | 对应 MCP 工具 |
|---|---|
| `<获取 hackernews 当前 top <N> 条故事>` | `hn_get_top_stories` |
| `<获取 hackernews 最新 <N> 条故事>` | `hn_get_new_stories` |
| `<获取 hackernews best <N> 条故事>` | `hn_get_best_stories` |
| `<获取 hackernews item <id> 的详情>` | `hn_get_item` |
| `<按关键词 <query> 搜索 hackernews 故事>` | `hn_search_by_keyword` |
| `<获取 hackernews 最新索引快照>` | `hn_get_latest_index` |
| `<抓取 hackernews 故事 <id> 的详情并写入缓存+实体注册>` | `hn_fetch_detailed_story` |

### Package 源 npm+PyPI(5 个模板)

| 语义模板 | 对应 MCP 工具 |
|---|---|
| `<搜索 npm 上 <关键词> 的包,前 <N> 个>` | `pkg_search_npm` |
| `<获取 npm 包 <name> 的详细信息>` | `pkg_get_npm_detail` |
| `<搜索 pypi 上 <关键词> 的包,前 <N> 个>` | `pkg_search_pypi` |
| `<获取 pypi 包 <name> 的详细信息>` | `pkg_get_pypi_detail` |
| `<抓取 <npm/pypi> 包 <name> 的详情并写入缓存+实体注册>` | `pkg_fetch_detail` |

### Papers with Code 源(6 个模板)

| 语义模板 | 对应 MCP 工具 |
|---|---|
| `<搜索 paperswithcode 上 <关键词> 的论文>` | `pwc_search_papers` |
| `<获取 paperswithcode 论文 <paper_id> 的详情>` | `pwc_get_paper_detail` |
| `<查询 paperswithcode 上 <task> 的 SOTA 模型>` | `pwc_get_sota` |
| `<搜索 paperswithcode 上的任务 <关键词>>` | `pwc_search_tasks` |
| `<搜索 paperswithcode 上的数据集 <关键词>>` | `pwc_search_datasets` |
| `<抓取 paperswithcode 论文 <paper_id> 的详情并写入缓存+实体注册>` | `pwc_fetch_paper_detail` |

### Hugging Face 源(9 个模板)

| 语义模板 | 对应 MCP 工具 |
|---|---|
| `<搜索 huggingface 上 <关键词> 的模型>` | `hf_search_models` |
| `<获取 huggingface 模型 <model_id> 的信息>` | `hf_get_model_info` |
| `<读取 huggingface 模型 <model_id> 的 README>` | `hf_get_model_readme` |
| `<搜索 huggingface 上 <关键词> 的数据集>` | `hf_search_datasets` |
| `<获取 huggingface 数据集 <dataset_id> 的信息>` | `hf_get_dataset_info` |
| `<获取 huggingface 当前 trending 模型>` | `hf_get_trending_models` |
| `<搜索 huggingface 上 <关键词> 的 space>` | `hf_search_spaces` |
| `<抓取 huggingface 模型 <model_id> 的详情并写入缓存+实体注册>` | `hf_fetch_model_detail` |
| `<抓取 huggingface 数据集 <dataset_id> 的详情并写入缓存+实体注册>` | `hf_fetch_dataset_detail` |

### Semantic Scholar 源(7 个模板)

| 语义模板 | 对应 MCP 工具 |
|---|---|
| `<搜索 semanticscholar 上 <关键词> 的论文>` | `s2_search_papers` |
| `<获取 semanticscholar 论文 <paper_id> 的详情>` | `s2_get_paper_detail` |
| `<查询 semanticscholar 论文 <paper_id> 的引用列表>` | `s2_get_citations` |
| `<查询 semanticscholar 论文 <paper_id> 的参考文献>` | `s2_get_references` |
| `<获取 semanticscholar 作者 <author_id> 的论文列表>` | `s2_get_author_papers` |
| `<搜索 semanticscholar 上的作者 <关键词>>` | `s2_search_author` |
| `<抓取 semanticscholar 论文 <paper_id> 的详情并写入缓存+实体注册>` | `s2_fetch_paper_detail` |

### Stack Overflow 源(6 个模板)

| 语义模板 | 对应 MCP 工具 |
|---|---|
| `<搜索 stackoverflow 上 <query> 的问题>` | `so_search_questions` |
| `<获取 stackoverflow 问题 <question_id> 的详情>` | `so_get_question_detail` |
| `<获取 stackoverflow 问题 <question_id> 的 top <N> 回答>` | `so_get_top_answers` |
| `<按 tags <tags> 搜索 stackoverflow 问题>` | `so_search_by_tags` |
| `<按标题 <title> 查找 stackoverflow 相似问题>` | `so_get_similar` |
| `<抓取 stackoverflow 问题 <question_id> 的详情并写入缓存+实体注册>` | `so_fetch_question_detail` |

### llama.cpp server 集成示例

```powershell
# MCP 配置 mcp_config.json
{
  "mcpServers": {
    "research": {
      "type": "stdio",
      "command": "D:/DeerFlow/DeerFlow++/build/Release/research-mcp.exe",
      "args": [
        "--port", "8765",
        "--gh-profile", "./profiles/gh",
        "--arxiv-profile", "./profiles/arxiv",
        "--hn-profile", "./profiles/hn",
        "--pkg-profile", "./profiles/pkg",
        "--pwc-profile", "./profiles/pwc",
        "--hf-profile", "./profiles/hf",
        "--s2-profile", "./profiles/s2",
        "--so-profile", "./profiles/so",
        "--proxy", "http://127.0.0.1:7897"
      ],
      "env": { "GITHUB_TOKEN": "ghp_xxx" }
    }
  }
}

# 启动 llama.cpp server (HTTP 模式 + MCP)
llama-server.exe ^
  -m models/qwen2.5-14b-instruct-q5_k_m.gguf ^
  --port 8080 ^
  --mcp-config mcp_config.json ^
  --mcp-protocol http
```

### 语义驱动调用链示例

**用户请求**:`帮我分析 github 项目 langchain-ai/langchain 在最近3个月内的相关活跃信息`

```
1. <获取 github 项目 langchain-ai/langchain 的基本信息>
   → github_get_repo_info(owner="langchain-ai", repo="langchain")

2. <枚举 github 项目 langchain-ai/langchain 的所有分支>
   → github_get_branches(owner="langchain-ai", repo="langchain")

3. <分析 github 项目 langchain-ai/langchain 在 main 的最近 50 条提交>
   → github_get_commits(owner="langchain-ai", repo="langchain", branch="main", limit=50)

4. <查询 github 项目 langchain-ai/langchain 的 issues,状态 all,前 30 条>
   → github_get_issues(owner="langchain-ai", repo="langchain", state="all", limit=30)

5. <查询 github 项目 langchain-ai/langchain 的 PR,状态 all,前 30 条>
   → github_get_pull_requests(owner="langchain-ai", repo="langchain", state="all", limit=30)
```

**跨源用户请求**:`研究 bert 模型的论文、实现、使用情况和社区讨论`

```
1. <搜索 arxiv 上 bert 的论文>
   → arxiv_search_papers(query="bert", max_results=5)

2. <抓取 arxiv 论文 1810.04805 的详情并写入缓存+实体注册>
   → arxiv_fetch_paper_detail(arxiv_id="1810.04805")

3. <搜索 huggingface 上 bert 的模型>
   → hf_search_models(query="bert")

4. <抓取 huggingface 模型 bert-base-uncased 的详情并写入缓存+实体注册>
   → hf_fetch_model_detail(model_id="bert-base-uncased")

5. <搜索 stackoverflow 上 bert fine-tuning 的问题>
   → so_search_questions(query="bert fine-tuning")

6. <搜索 npm 上 bert 的包>
   → pkg_search_npm(query="bert")
```

## 真实网络端到端验证(HTTP Server 状态)

### 验证结果(2026-08-06)

| 验证项 | 结果 | 说明 |
|---|---|---|
| GET `/` | ✅ | 返回 8 源状态(GitHub=true,其余需 --xxx-profile) |
| GET `/tools` | ✅ | 返回 65 个工具(完整列表) |
| POST `/mcp` JSON-RPC `tools/list` | ✅ | 返回 65 个工具,JSON-RPC id=1 正确匹配 |
| 缓存层 188 项烟雾测试 | ✅ pass=188 fail=0 | EXIT_CODE=0 |
| 完整 8 源 fetch 回调 | ✅ | 8 源 WebView2 session 全部 ready,proxy=http://127.0.0.1:7897 |
| `github_module_timeline_analysis` ingest_first=false | ✅ | timeline_count=3 (research-mcp 仓库 src/tools.cpp) |
| `github_module_timeline_analysis` ingest_first=true + branch | ✅ | timeline_count=35 (cxvision 仓库 codex/cxcore-integration 分支, FastMatch 签名匹配) |

### branch 参数验证(非默认分支)

以 `Sean-Cai-X/cxvision` 的 `codex/cxcore-integration` 分支为例,分析 FastMatch 相关文件更新时间线:

```json
{
  "name": "github_module_timeline_analysis",
  "arguments": {
    "owner": "Sean-Cai-X",
    "repo": "cxvision",
    "target_type": "signature",
    "signature_regex": "FastMatch",
    "branch": "codex/cxcore-integration",
    "ingest_first": true,
    "time_range": "1y"
  }
}
```

返回 35 条 timeline 记录,关键文件包括:
- `cximage/FastMatch.h` / `FastMatch.cpp` — 核心算法
- `cximage/FastMatchGridClassAdapter.h/.cpp` — 网格模式适配器
- `cximage/CxFastMatchRuntimeCapture.h/.cpp` — 运行时捕获
- `cxparser/cxscript/module/cximage/frozen/fastmatch/` — 冻结脚本
- `cxparser/cxscript/module/cximage/diagnostic/fastmatch/` — 诊断脚本

> 注意: 未配置 `GITHUB_TOKEN` 时,GitHub API 速率限制为 60 次/小时。commits 列表可成功获取,但逐条抓取 commit 详情可能触发 HTTP 403。此时 `ingest_status=failed`,`ingest_error` 会报告具体错误,timeline 仍返回已获取的列表数据。配置 `GITHUB_TOKEN` 可解除此限制。

### 执行命令

```powershell
# 启动完整 8 源 HTTP Server(需先创建 profiles 目录)
mkdir -Force ./profiles/gh,./profiles/arxiv,./profiles/hn,./profiles/pkg,./profiles/pwc,./profiles/hf,./profiles/s2,./profiles/so

.\build\Release\research-mcp.exe --port 8765 `
  --gh-profile    ./profiles/gh `
  --arxiv-profile ./profiles/arxiv `
  --hn-profile    ./profiles/hn `
  --pkg-profile   ./profiles/pkg `
  --pwc-profile   ./profiles/pwc `
  --hf-profile    ./profiles/hf `
  --s2-profile    ./profiles/s2 `
  --so-profile    ./profiles/so `
  --proxy http://127.0.0.1:7897
```

## 通用 GN/Ninja 编译流水线模板(完成案例)

适配 **任意 GN 工程**(LLVM / Chromium 子工程 / 自定义 GN 项目), 统一抽象编译语义, 内置 **安全缓存 / 哈希校验 / 制品清单 / Runner 加固**。可直接交付 Trae 标准化落地; 通过环境变量切换工程路径、编译参数、输出目录, 一套模板多项目复用。

### 设计思想

1. 把「源码位置、GN 参数文件、产物目录」全部抽成环境变量, 不写死 LLVM 相关路径
2. 缓存 Key 绑定三大因子: **系统标识 + GN 参数哈希 + 源码版本哈希**
3. 标准化四阶段: **环境加固 → 构建 → 制品平铺/组装 → 功能校验 + 哈希清单生成**
4. 所有 Action Pin commit; 最小权限; 并发防缓存踩踏

### 仓库标准目录规范(统一所有 GN 项目)

```
repo/
├── build/
│   ├── release.args    # GN Release 参数
│   └── debug.args      # GN Debug 参数(可选)
├── scripts/
│   └── verify_artifact.sh
├── .github/workflows/
│   └── gn-universal-build.yml
└── [GN 源码根目录: BUILD.gn / .gn]
```

### 流水线文件清单

| 文件 | 用途 |
|---|---|
| [.github/workflows/gn-universal-build.yml](.github/workflows/gn-universal-build.yml) | 通用 GN/Ninja 编译流水线 |
| [scripts/verify_artifact.sh](scripts/verify_artifact.sh) | 下游制品完整性校验脚本 |
| [build/release.args](build/release.args) | GN Release 参数模板 |

### 流水线 11 步标准流程

1. **Harden Runner** — `step-security/harden-runner@0d381219` 网络出口审计
2. **Checkout source** — `actions/checkout@34e11487` fetch-depth=0
3. **Calculate build fingerprint** — 计算 `GIT_HEAD` + `ARGS_HASH`(参数文件 SHA256)
4. **Install system dependencies** — `build-essential git python3`
5. **Restore build cache** — `actions/cache@269d9d88` Key 三重绑定 `os-gn-head-args_hash`
6. **Full clean (No cache mode)** — `ENABLE_FULL_CLEAN=true` 时禁用缓存
7. **GN generate build.ninja** — `gn gen out/release --args-file=build/release.args`
8. **Ninja build all targets** — `ninja -j $(nproc) -C out/release`
9. **Assemble deploy directory** — `ninja install` + 平铺到 `deploy_output/`
10. **Sanity verify built binaries** — 项目专属自检(可替换)
11. **Generate artifact sha256 manifest** — `find | sort | xargs sha256sum` 生成全局清单
12. **Upload build artifacts** — `actions/upload-artifact@6546e369` 保留 14 天

### build/release.args 通用 GN 基础模板

```gn
is_optimized = true
is_debug = false
use_debug_dwarf = false
symbol_level = 0
strip_output = true
```

### 下游校验脚本用法

```bash
# 下游流水线拉取制品后强制调用
./scripts/verify_artifact.sh ./deploy_output ../artifact_manifest.sha256
```

校验流程:
1. 文件哈希清单逐条 `sha256sum --check`
2. 任意不匹配立即终止
3. 通过后追加项目专属二进制自检(可选)

### Trae 落地强制约束(标准化规则)

1. **禁止硬编码项目路径**, 全部通过 workflow env 顶层配置
2. 发布流水线调用 workflow 时, 传入 `ENABLE_FULL_CLEAN: true`, 放弃缓存、纯净构建
3. 所有第三方 Action 必须锁定完整 commit SHA, 禁止 `@vX`
4. 下游使用制品前 **必须执行 verify_artifact.sh**, 校验不通过直接终止
5. 缓存 Key 三重绑定: 系统 + git commit + args 文件哈希, 避免参数变更后加载旧产物
6. 构建全程不使用 root/sudo

### 安全特性清单

| 特性 | 实现 |
|---|---|
| **哈希校验** | 全局 SHA256 清单 + 下游强制 verify |
| **安全缓存** | Key 三重绑定 + 并发锁 + 全量清理模式 |
| **Runner 硬化** | step-security/harden-runner egress-policy=audit |
| **最小权限** | `contents: read` + `pull-requests: read` |
| **Action Pin SHA** | 全部锁定 commit SHA, 禁止 `@vX` 浮动标签 |
| **构建完整性自检** | Sanity verify 步骤 + manifest signature |

### 多项目复用技巧

不同仓库只需要修改 workflow 顶部 `env` 区块, 其余逻辑完全不用改动:

```yaml
env:
  SRC_ROOT: "."                     # 改这里
  GN_OUT_DIR: out/release           # 改这里
  GN_ARGS_FILE: build/release.args  # 改这里
  ARTIFACT_ROOT: ./deploy_output    # 改这里
```

### 扩展可选能力(后续迭代)

- 支持多配置并行构建(Debug / Release 矩阵)
- 接入 sccache 加速增量编译
- 增加容器隔离 `container: ubuntu:22.04`
- 制品启用 gh attestation 签名(SLSA 合规)

## 许可

随 DeerFlow 仓库分发。
