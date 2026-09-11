cmake_minimum_required(VERSION 3.21)
project(research-mcp VERSION 0.3.5 LANGUAGES C CXX)

# ============================================================================
# 全局编译标准 + MSVC 静态 CRT 统一（全局唯一一处）
# ============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    # 强制静态 CRT：Release=/MT、Debug=/MTd，和发布目标一致
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
    # 消除不安全函数、废弃 API 告警
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        _WINSOCK_DEPRECATED_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

# ============================================================================
# 浏览器后端开关: WEBVIEW2 (默认, Windows only) | CDP (跨平台, 零GUI依赖)
# ============================================================================
option(RESEARCH_MCP_BROWSER_BACKEND "Browser backend: WEBVIEW2 or CDP" WEBVIEW2)

# ============================================================================
# FetchContent 依赖：仅 nlohmann/json（curl 用本地源码，不走 FetchContent）
# ============================================================================
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# --- nlohmann/json (单头 header-only, GitHub tag v3.11.3) ---
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "" FORCE)
set(JSON_MultipleHeaders OFF CACHE INTERNAL "" FORCE)
FetchContent_MakeAvailable(json)

# ======================================================================
# curl 本地源码编译配置（必须全部在 add_subdirectory 之前设置）
# ======================================================================
# 基础构建选项
set(BUILD_CURL_EXE       OFF CACHE BOOL "" FORCE)  # 不编译 curl 命令行工具
set(BUILD_CURL_TESTS     OFF CACHE BOOL "" FORCE)  # 不编译测试
set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)  # 编译静态库
set(CURL_STATICLIB        ON  CACHE BOOL "" FORCE)  # 静态库模式
set(CURL_ENABLE_MANUAL    OFF CACHE BOOL "" FORCE)  # 不生成 man 手册

# 禁用非必要应用层协议（CURL_DISABLE_ 前缀，设 ON 为禁用）
set(CURL_DISABLE_LDAP     ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_IMAP     ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_POP3     ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_SMTP     ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_GOPHER   ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_DICT     ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_TELNET   ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_RTSP     ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_FTP      ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_FILE     ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_TFTP     ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_MQTT     ON CACHE BOOL "" FORCE)

# ===== 核心：禁用所有可选第三方依赖（CURL_USE_ 前缀，设 OFF 为禁用）=====
# 压缩算法依赖
set(CURL_USE_ZLIB         OFF CACHE BOOL "" FORCE)  # 禁用 zlib/gzip
set(CURL_USE_BROTLI       OFF CACHE BOOL "" FORCE)  # 禁用 brotli
set(CURL_USE_ZSTD         OFF CACHE BOOL "" FORCE)  # 禁用 zstd

# 协议扩展依赖
set(CURL_USE_NGHTTP2      OFF CACHE BOOL "" FORCE)  # 禁用 HTTP/2
set(CURL_USE_NGTCP2       OFF CACHE BOOL "" FORCE)  # 禁用 HTTP/3

# 域名与公共后缀依赖
set(CURL_USE_LIBIDN2      OFF CACHE BOOL "" FORCE)  # 禁用 IDN 国际化域名
set(CURL_USE_LIBPSL       OFF CACHE BOOL "" FORCE)  # 禁用公共后缀列表

# 加密与认证依赖（Windows 用原生 Schannel 替代）
set(CURL_USE_OPENSSL      OFF CACHE BOOL "" FORCE)
set(CURL_USE_GSSAPI       OFF CACHE BOOL "" FORCE)
set(CURL_USE_KERBEROS     OFF CACHE BOOL "" FORCE)
set(CURL_USE_AUTH         OFF CACHE BOOL "" FORCE)  # 简化认证

# Windows 下启用系统原生 Schannel 做 TLS，无需额外加密库
if(WIN32)
    set(CURL_USE_SCHANNEL    ON  CACHE BOOL "" FORCE)
endif()

# 引入本地 curl 源码（全局唯一一处，所有配置必须在这行之前）
add_subdirectory(third_party/curl-8.20.0)

# ============================================================================
# SQLite3：仓库已存 amalgamation 源码
# ============================================================================
set(SQLITE_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/sqlite)
if(NOT EXISTS ${SQLITE_INCLUDE_DIR}/sqlite3.h)
    message(FATAL_ERROR "SQLite3 not found at third_party/sqlite/sqlite3.h")
endif()

# ============================================================================
# 浏览器后端条件配置
# ============================================================================
if(RESEARCH_MCP_BROWSER_BACKEND STREQUAL "WEBVIEW2")
    message(STATUS "=== Browser backend: WebView2 (Windows only) ===")
    if(NOT WIN32)
        message(FATAL_ERROR "WebView2 backend only supports Windows. Use -DRESEARCH_MCP_BROWSER_BACKEND=CDP for cross-platform build.")
    endif()

    # 下载 WebView2 NuGet SDK（仅 WebView2 模式执行）
    FetchContent_Declare(webview2
        URL https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2792.45
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_GetProperties(webview2)
    if(NOT webview2_POPULATED)
        FetchContent_Populate(webview2)
    endif()

    set(WEBVIEW2_INCLUDE_DIR ${webview2_SOURCE_DIR}/build/native/include)
    set(WEBVIEW2_LIB_PATH     ${webview2_SOURCE_DIR}/build/native/x64/WebView2Loader.dll.lib)
    set(WEBVIEW2_DLL_PATH     ${webview2_SOURCE_DIR}/runtimes/win-x64/native/WebView2Loader.dll)

    if(NOT EXISTS ${WEBVIEW2_INCLUDE_DIR}/WebView2.h)
        message(FATAL_ERROR "WebView2.h not found at ${WEBVIEW2_INCLUDE_DIR}")
    endif()

    set(BROWSER_BACKEND_SOURCES
        src/webview_session.cpp
        src/webview_client.cpp
    )
    set(BROWSER_COMPILE_DEF RESEARCH_MCP_USE_WEBVIEW2)

elseif(RESEARCH_MCP_BROWSER_BACKEND STREQUAL "CDP")
    message(STATUS "=== Browser backend: CDP Chrome (cross-platform, zero GUI) ===")
    set(BROWSER_BACKEND_SOURCES
        src/browser_session_cdp.cpp
    )
    set(BROWSER_COMPILE_DEF RESEARCH_MCP_USE_CDP)

else()
    message(FATAL_ERROR "Unknown RESEARCH_MCP_BROWSER_BACKEND: ${RESEARCH_MCP_BROWSER_BACKEND}. Use WEBVIEW2 or CDP.")
endif()

# ============================================================================
# 主目标定义
# ============================================================================
add_executable(research-mcp
    src/main.cpp
    src/mcp_server.cpp
    src/http_server.cpp
    src/tools.cpp
    src/github_client.cpp
    src/cache_manager.cpp
    src/source_fusion.cpp
    ${BROWSER_BACKEND_SOURCES}
    src/browser_config.cpp
    src/webview_helpers.cpp
    src/curl_http_client.cpp
    src/arxiv_tools.cpp
    src/hackernews_tools.cpp
    src/hn_firebase.cpp
    src/package_tools.cpp
    src/paperswithcode_tools.cpp
    src/huggingface_tools.cpp
    src/semanticscholar_tools.cpp
    src/stackoverflow_tools.cpp
    src/formatters.cpp
    src/string_utils.cpp
    src/research_deep_dive.cpp
    src/datasource_registry.cpp
    src/kiwix_source.cpp
    src/git_raw_source.cpp
    src/github_wiki_source.cpp
    src/web_crawler_source.cpp
    src/github_api_source.cpp
    src/arxiv_source.cpp
    src/web_search_source.cpp
    src/local_fs_source.cpp
    src/git_clone_source.cpp
    src/wiki_explorer.cpp
    src/focus_tools.cpp
    src/focus_engine.cpp
    src/focus_attr_engine.cpp
    src/focus_track_engine.cpp
    src/focus_web_search.cpp
    third_party/sqlite/sqlite3.c
)

target_include_directories(research-mcp PRIVATE
    include
    ${SQLITE_INCLUDE_DIR}
)

target_compile_definitions(research-mcp PRIVATE ${BROWSER_COMPILE_DEF})

# WebView2 专属头文件（仅 WebView2 模式加入包含路径）
if(RESEARCH_MCP_BROWSER_BACKEND STREQUAL "WEBVIEW2")
    target_include_directories(research-mcp PRIVATE ${WEBVIEW2_INCLUDE_DIR})
endif()

# ============================================================================
# 统一链接配置
# ============================================================================
target_link_libraries(research-mcp PRIVATE
    nlohmann_json::nlohmann_json
    CURL::libcurl
)

# Windows 系统库
if(WIN32)
    target_link_libraries(research-mcp PRIVATE
        ws2_32
        wldap32
        winmm
        crypt32
        advapi32
        user32
        bcrypt
        kernel32
    )
else()
    target_link_libraries(research-mcp PRIVATE pthread dl)
endif()

# WebView2 静态库（仅 WebView2 模式链接）
if(RESEARCH_MCP_BROWSER_BACKEND STREQUAL "WEBVIEW2" AND WIN32)
    target_link_libraries(research-mcp PRIVATE ${WEBVIEW2_LIB_PATH})
endif()

# ============================================================================
# MSVC 编译选项
# ============================================================================
if(MSVC)
    set_property(TARGET research-mcp PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_compile_options(research-mcp PRIVATE /MT /W3 /EHsc /utf-8)
endif()

# ============================================================================
# POST_BUILD：复制运行时 DLL
# ============================================================================
if(RESEARCH_MCP_BROWSER_BACKEND STREQUAL "WEBVIEW2" AND WIN32)
    add_custom_command(TARGET research-mcp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${WEBVIEW2_DLL_PATH}
            $<TARGET_FILE_DIR:research-mcp>/WebView2Loader.dll
    )
endif()

# ============================================================================
# 诊断输出
# ============================================================================
message(STATUS "")
message(STATUS "=== research-mcp 配置汇总 ===")
message(STATUS "  Backend:       ${RESEARCH_MCP_BROWSER_BACKEND}")
message(STATUS "  CURL target:   CURL::libcurl (静态库, Schannel TLS)")
message(STATUS "  SQLITE:        ${SQLITE_INCLUDE_DIR}")
message(STATUS "  nlohmann:      ${nlohmann_json_DIR}")
message(STATUS "")

# ============================================================================
# 测试
# ============================================================================
enable_testing()
add_subdirectory(tests)
