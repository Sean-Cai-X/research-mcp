cmake_minimum_required(VERSION 3.16)
project(research-mcp VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================================
# 浏览器后端开关: WEBVIEW2 (默认, Windows only) | CDP (跨平台, 零GUI依赖)
# ============================================================================
option(RESEARCH_MCP_BROWSER_BACKEND "Browser backend: WEBVIEW2 or CDP" WEBVIEW2)

# === nlohmann/json: 优先用 vcpkg,否则用单 header(随仓库分发) ===
find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    set(JSON_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/json/include)
    if(NOT EXISTS ${JSON_INCLUDE_DIR}/nlohmann/json.hpp)
        message(WARNING "nlohmann/json not found via vcpkg, will use single-header at ${JSON_INCLUDE_DIR}")
    endif()
endif()

# === SQLite3: amalgamation 直接编译入 exe (无外部依赖) ===
set(SQLITE_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/sqlite)
if(NOT EXISTS ${SQLITE_INCLUDE_DIR}/sqlite3.h)
    message(FATAL_ERROR "SQLite3 not found at third_party/sqlite/sqlite3.h")
endif()

# === libcurl: 优先 vcpkg,否则用 third_party/curl 官方预编译包 ===
# WebView2 后端: 用于 CurlHttpClient(GitHub REST API)
# CDP 后端: 额外用于 WebSocket (curl v8.20+ 内置 WebSocket API)
find_package(CURL CONFIG QUIET)
if(NOT CURL_FOUND)
    file(GLOB CURL_DIRS "${CMAKE_SOURCE_DIR}/third_party/curl/curl-*-win64-mingw")
    if(CURL_DIRS)
        list(SORT CURL_DIRS COMPARE NATURAL)
        list(REVERSE CURL_DIRS)
        list(GET CURL_DIRS 0 CURL_ROOT_DIR)
        set(CURL_INCLUDE_DIR ${CURL_ROOT_DIR}/include)
        set(CURL_LIB_DIR ${CURL_ROOT_DIR}/lib)
        set(CURL_BIN_DIR ${CURL_ROOT_DIR}/bin)
        if(NOT EXISTS ${CURL_INCLUDE_DIR}/curl/curl.h)
            message(FATAL_ERROR "libcurl headers not found at ${CURL_INCLUDE_DIR}/curl/curl.h")
        endif()
        if(NOT EXISTS ${CURL_LIB_DIR}/libcurl.lib)
            message(FATAL_ERROR
                "libcurl.lib not found. Generate it with MSVC lib.exe:\n"
                "  lib /machine:x64 /def:\"${CURL_BIN_DIR}/libcurl-x64.def\" /out:\"${CURL_LIB_DIR}/libcurl.lib\"")
        endif()
        message(STATUS "libcurl (prebuilt): ${CURL_ROOT_DIR}")
    else()
        message(FATAL_ERROR
            "libcurl not found. Download official Windows build from https://curl.se/windows/ \n"
            "and extract to: ${CMAKE_SOURCE_DIR}/third_party/curl/")
    endif()
endif()

# ============================================================================
# WebView2 后端: Windows only + WebView2 SDK
# ============================================================================
if(RESEARCH_MCP_BROWSER_BACKEND STREQUAL "WEBVIEW2")
    message(STATUS "=== Browser backend: WebView2 (Windows only) ===")

    if(NOT WIN32)
        message(FATAL_ERROR "WebView2 backend only supports Windows. Use -DRESEARCH_MCP_BROWSER_BACKEND=CDP for cross-platform build.")
    endif()

    # === WebView2 SDK: 优先 vcpkg,否则用 third_party/WebView2 ===
    find_package(webview2 CONFIG QUIET)
    if(NOT webview2_FOUND)
        set(WEBVIEW2_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/WebView2/include)
        set(WEBVIEW2_LIB_DIR ${CMAKE_SOURCE_DIR}/third_party/WebView2/lib/x64)
        if(NOT EXISTS ${WEBVIEW2_INCLUDE_DIR}/WebView2.h)
            message(FATAL_ERROR
                "WebView2 SDK not found. Please download Microsoft.Web.WebView2 NuGet package "
                "and extract to: ${CMAKE_SOURCE_DIR}/third_party/WebView2\n"
                "Expected layout:\n"
                "  third_party/WebView2/include/WebView2.h\n"
                "  third_party/WebView2/include/WebView2Environment.h\n"
                "  third_party/WebView2/lib/x64/WebView2Loader.dll.lib\n"
                "  third_party/WebView2/redist/x64/WebView2Loader.dll")
        endif()
    endif()

    set(BROWSER_BACKEND_SOURCES
        src/webview_session.cpp
        src/webview_client.cpp
    )
    set(WEBVIEW2_INC_CONDITION ${WEBVIEW2_INCLUDE_DIR})
    set(NEEDS_WEBVIEW2_LIB TRUE)
    set(NEEDS_WINDOWS_H TRUE)
    set(BROWSER_COMPILE_DEF RESEARCH_MCP_USE_WEBVIEW2)

# ============================================================================
# CDP 后端: 跨平台 + 零 GUI 依赖 + libcurl WebSocket
# ============================================================================
elseif(RESEARCH_MCP_BROWSER_BACKEND STREQUAL "CDP")
    message(STATUS "=== Browser backend: CDP Chrome (cross-platform, zero GUI) ===")

    set(BROWSER_BACKEND_SOURCES
        src/browser_session_cdp.cpp
    )
    set(WEBVIEW2_INC_CONDITION "")  # CDP 不链 WebView2
    set(NEEDS_WEBVIEW2_LIB FALSE)
    set(NEEDS_WINDOWS_H FALSE)       # 保留条件化 Windows 进程管理,不强制
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
    # 9-source unified search architecture
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
    ${JSON_INCLUDE_DIR}
    ${WEBVIEW2_INC_CONDITION}
    ${SQLITE_INCLUDE_DIR}
    ${CURL_INCLUDE_DIR}
)

# 后端编译宏
target_compile_definitions(research-mcp PRIVATE ${BROWSER_COMPILE_DEF})

if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(research-mcp PRIVATE nlohmann_json::nlohmann_json)
endif()

# === WebView2 链接 (仅 WEBVIEW2 后端) ===
if(NEEDS_WEBVIEW2_LIB)
    if(TARGET WebView2::WebView2)
        target_link_libraries(research-mcp PRIVATE WebView2::WebView2)
    else()
        target_link_libraries(research-mcp PRIVATE ${WEBVIEW2_LIB_DIR}/WebView2Loader.dll.lib)
    endif()
endif()

# === libcurl 链接 ===
if(TARGET CURL::libcurl)
    target_link_libraries(research-mcp PRIVATE CURL::libcurl)
elseif(EXISTS ${CURL_LIB_DIR}/libcurl.lib)
    target_link_libraries(research-mcp PRIVATE ${CURL_LIB_DIR}/libcurl.lib)
endif()

# === 平台系统库 ===
if(WIN32)
    if(RESEARCH_MCP_BROWSER_BACKEND STREQUAL "WEBVIEW2")
        # WebView2 后端: 完整 Windows 系统库
        target_link_libraries(research-mcp PRIVATE
            user32          # 窗口 API
            ole32           # COM
            oleaut32        # COM 辅助
            runtimeobject    # Microsoft::WRL::Callback
            ws2_32          # WinSock(HTTP 模式)
            crypt32         # libcurl HTTPS 证书存储
            bcrypt          # libcurl Schannel/加密原语
        )
    else()
        # CDP 后端 (Windows): 保留 kernel32 (进程管理), ws2_32 (WinSock), 去掉 COM 相关
        target_link_libraries(research-mcp PRIVATE
            kernel32        # CreateProcess / TerminateProcess
            ws2_32          # WinSock (curl + CDP HTTP)
            crypt32         # libcurl HTTPS 证书存储
            bcrypt          # libcurl Schannel/加密原语
        )
    endif()
else()
    # Linux / macOS: pthread + dl
    target_link_libraries(research-mcp PRIVATE pthread dl)
endif()

# === 运行时 DLL 复制 ===
# WebView2Loader.dll (仅 WEBVIEW2 后端)
if(NEEDS_WEBVIEW2_LIB)
    set(WEBVIEW2_DLL_PATH ${CMAKE_SOURCE_DIR}/third_party/WebView2/redist/x64/WebView2Loader.dll)
    if(EXISTS ${WEBVIEW2_DLL_PATH})
        add_custom_command(TARGET research-mcp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${WEBVIEW2_DLL_PATH}
            $<TARGET_FILE_DIR:research-mcp>/WebView2Loader.dll)
    endif()
endif()

# libcurl-x64.dll 和 CA 证书 (所有后端)
if(EXISTS ${CURL_BIN_DIR}/libcurl-x64.dll)
    add_custom_command(TARGET research-mcp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CURL_BIN_DIR}/libcurl-x64.dll
        $<TARGET_FILE_DIR:research-mcp>/libcurl-x64.dll
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CURL_BIN_DIR}/curl-ca-bundle.crt
        $<TARGET_FILE_DIR:research-mcp>/curl-ca-bundle.crt)
endif()

# === MSVC 编译选项 ===
if(MSVC)
    set_property(TARGET research-mcp PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_compile_options(research-mcp PRIVATE /MT /W3 /EHsc /utf-8)
    target_compile_definitions(research-mcp PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
endif()

# 测试
enable_testing()
add_subdirectory(tests)
