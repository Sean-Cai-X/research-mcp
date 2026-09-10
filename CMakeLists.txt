cmake_minimum_required(VERSION 3.21)
project(research-mcp VERSION 0.3.5 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================================
# 浏览器后端开关: WEBVIEW2 (默认, Windows only) | CDP (跨平台, 零GUI依赖)
# ============================================================================
option(RESEARCH_MCP_BROWSER_BACKEND "Browser backend: WEBVIEW2 or CDP" WEBVIEW2)

# ============================================================================
# FetchContent: 所有依赖从源码/官方发行包拉取,零预编译二进制进仓库
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

# --- curl 8.20.x (GitHub tag curl-8_20_0, 内置 WebSocket API) ---
FetchContent_Declare(curl
    GIT_REPOSITORY https://github.com/curl/curl.git
    GIT_TAG curl-8_20_0
    GIT_SHALLOW TRUE
)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# 协议精简: 只保留 HTTP/HTTPS/WS/WSS (CDP 需要 WebSocket)
set(CURL_DISABLE_PROTOCOLS "DICT;FILE;GOPHER;IMAP;LDAP;MOPROXY;POP3;RTSP;SMB;SMTP;TELNET;TFTP" CACHE STRING "" FORCE)

# 可选依赖全关 (FetchContent 不递归拉子依赖, 都关了省时间)
set(CURL_USE_LIBPSL     OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBIDN2    OFF CACHE BOOL "" FORCE)
set(CURL_USE_ZLIB       OFF CACHE BOOL "" FORCE)
set(CURL_USE_BROTLI     OFF CACHE BOOL "" FORCE)
set(CURL_USE_ZSTD       OFF CACHE BOOL "" FORCE)
set(CURL_USE_NGHTTP2    OFF CACHE BOOL "" FORCE)
set(CURL_USE_NGTCP2     OFF CACHE BOOL "" FORCE)
set(CURL_USE_AUTH       OFF CACHE BOOL "" FORCE)  # 简化认证
set(CURL_ENABLE_MANUAL  OFF CACHE BOOL "" FORCE)  # 不需要 man 手册

if(WIN32)
    set(CURL_USE_SCHANNEL ON CACHE BOOL "Use Windows Schannel for TLS" FORCE)
else()
    # Linux/macOS: curl 依赖系统 OpenSSL (apt install libssl-dev)
    set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
    set(OPENSSL_ROOT_DIR "" CACHE PATH "" FORCE)
endif()

# 拉 nlohmann + curl (CMake 项目, FetchContent_MakeAvailable 自动 add_subdirectory)
FetchContent_MakeAvailable(json curl)

message(STATUS "=== FetchContent deps ready ===")
message(STATUS "  nlohmann_json:  target=${nlohmann_json_DIR}")
message(STATUS "  curl:           target=${CURL_DIR}")

# --- SQLite3: 仓库已存 amalgamation 源码 (third_party/sqlite/sqlite3.c) ---
set(SQLITE_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/sqlite)
if(NOT EXISTS ${SQLITE_INCLUDE_DIR}/sqlite3.h)
    message(FATAL_ERROR "SQLite3 not found at third_party/sqlite/sqlite3.h")
endif()

# ============================================================================
# WebView2 后端: Windows only + FetchContent 下载 NuGet SDK
# ============================================================================
if(RESEARCH_MCP_BROWSER_BACKEND STREQUAL "WEBVIEW2")
    message(STATUS "=== Browser backend: WebView2 (Windows only) ===")

    if(NOT WIN32)
        message(FATAL_ERROR "WebView2 backend only supports Windows. Use -DRESEARCH_MCP_BROWSER_BACKEND=CDP for cross-platform build.")
    endif()

    # FetchContent 只下载, 不 add_subdirectory (NuGet 包非 CMake 项目)
    FetchContent_Declare(webview2
        URL https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2792.45
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_GetProperties(webview2)
    if(NOT webview2_POPULATED)
        FetchContent_Populate(webview2)
    endif()

    # NuGet 包 (zip) 解压后的布局:
    #   build/native/include/WebView2.h
    #   build/native/x64/WebView2Loader.dll.lib
    #   runtimes/win-x64/native/WebView2Loader.dll
    set(WEBVIEW2_INCLUDE_DIR ${webview2_SOURCE_DIR}/build/native/include)
    set(WEBVIEW2_LIB_PATH     ${webview2_SOURCE_DIR}/build/native/x64/WebView2Loader.dll.lib)
    set(WEBVIEW2_DLL_PATH     ${webview2_SOURCE_DIR}/runtimes/win-x64/native/WebView2Loader.dll)

    if(NOT EXISTS ${WEBVIEW2_INCLUDE_DIR}/WebView2.h)
        message(FATAL_ERROR "WebView2 SDK include/WebView2.h not found at ${WEBVIEW2_INCLUDE_DIR}")
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
# 目标定义
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

if(RESEARCH_MCP_BROWSER_BACKEND STREQUAL "WEBVIEW2")
    target_include_directories(research-mcp PRIVATE ${WEBVIEW2_INCLUDE_DIR})
endif()

target_compile_definitions(research-mcp PRIVATE ${BROWSER_COMPILE_DEF})

# --- 链接: nlohmann + curl (FetchContent 创建的 CMake targets) ---
target_link_libraries(research-mcp PRIVATE
    nlohmann_json::nlohmann_json
    CURL::libcurl
)

# --- WebView2 链接 (静态导入库, 非 CMake target) ---
if(RESEARCH_MCP_BROWSER_BACKEND STREQUAL "WEBVIEW2" AND WIN32)
    target_link_libraries(research-mcp PRIVATE ${WEBVIEW2_LIB_PATH})
endif()

# --- 平台系统库 ---
if(WIN32)
    if(RESEARCH_MCP_BROWSER_BACKEND STREQUAL "WEBVIEW2")
        target_link_libraries(research-mcp PRIVATE
            user32
            ole32
            oleaut32
            runtimeobject
            ws2_32
            crypt32
            bcrypt
        )
    else()
        target_link_libraries(research-mcp PRIVATE
            kernel32
            ws2_32
            crypt32
            bcrypt
        )
    endif()
else()
    target_link_libraries(research-mcp PRIVATE pthread dl)
endif()

# --- POST_BUILD: 复制运行时 DLL (WebView2 Loader) ---
if(RESEARCH_MCP_BROWSER_BACKEND STREQUAL "WEBVIEW2" AND WIN32)
    add_custom_command(TARGET research-mcp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${WEBVIEW2_DLL_PATH}
            $<TARGET_FILE_DIR:research-mcp>/WebView2Loader.dll
    )
endif()

# --- MSVC 编译选项 ---
if(MSVC)
    set_property(TARGET research-mcp PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_compile_options(research-mcp PRIVATE /MT /W3 /EHsc /utf-8)
    target_compile_definitions(research-mcp PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
endif()

# ============================================================================
# 诊断输出
# ============================================================================
message(STATUS "")
message(STATUS "=== research-mcp 配置汇总 ===")
message(STATUS "  Backend:       ${RESEARCH_MCP_BROWSER_BACKEND}")
message(STATUS "  CURL target:   CURL::libcurl (BUILD_SHARED_LIBS=${BUILD_SHARED_LIBS})")
message(STATUS "  SQLITE:        ${SQLITE_INCLUDE_DIR}")
message(STATUS "")

# 测试
enable_testing()
add_subdirectory(tests)
