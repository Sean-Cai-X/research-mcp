# ============================================================================
# 测试工程：独立编译源文件，不链接 research-mcp 主程序
# ============================================================================
add_executable(test_smoke
    test_smoke.cpp
    ${CMAKE_SOURCE_DIR}/src/github_client.cpp
    ${CMAKE_SOURCE_DIR}/src/curl_http_client.cpp
    ${CMAKE_SOURCE_DIR}/src/cache_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/source_fusion.cpp
    ${CMAKE_SOURCE_DIR}/src/formatters.cpp
    ${CMAKE_SOURCE_DIR}/src/string_utils.cpp
    ${CMAKE_SOURCE_DIR}/third_party/sqlite/sqlite3.c
)

# ===== 仅 WebView2 模式下才加入 WebView2 相关源码 =====
if(RESEARCH_MCP_BROWSER_BACKEND STREQUAL "WEBVIEW2")
    target_sources(test_smoke PRIVATE
        ${CMAKE_SOURCE_DIR}/src/webview_client.cpp
    )
endif()

# ============================================================================
# 包含目录
# ============================================================================
target_include_directories(test_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/sqlite
)

# 公共依赖：nlohmann_json
if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(test_smoke PRIVATE nlohmann_json::nlohmann_json)
endif()

# 公共依赖：curl 静态库
if(TARGET CURL::libcurl)
    target_link_libraries(test_smoke PRIVATE CURL::libcurl)
elseif(EXISTS ${CURL_LIB_DIR}/libcurl.lib)
    target_include_directories(test_smoke PRIVATE ${CURL_INCLUDE_DIR})
    target_link_libraries(test_smoke PRIVATE ${CURL_LIB_DIR}/libcurl.lib)
endif()

# ===== 仅 WebView2 模式下才加入 WebView2 头文件与链接库 =====
if(RESEARCH_MCP_BROWSER_BACKEND STREQUAL "WEBVIEW2")
    target_include_directories(test_smoke PRIVATE ${WEBVIEW2_INCLUDE_DIR})
    
    if(TARGET WebView2::WebView2)
        target_link_libraries(test_smoke PRIVATE WebView2::WebView2)
    else()
        target_link_libraries(test_smoke PRIVATE ${WEBVIEW2_LIB_PATH}/WebView2Loader.dll.lib)
    endif()
endif()

# ============================================================================
# Windows 系统库
# ============================================================================
if(WIN32)
    target_link_libraries(test_smoke PRIVATE
        user32
        ole32
        oleaut32
        runtimeobject
        crypt32
        bcrypt
        ws2_32
        advapi32
    )
endif()

# ============================================================================
# MSVC 编译选项（与主项目一致：静态 CRT）
# ============================================================================
if(MSVC)
    set_property(TARGET test_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_compile_options(test_smoke PRIVATE /MT /W3 /EHsc /utf-8)
    target_compile_definitions(test_smoke PRIVATE
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
        _WINSOCK_DEPRECATED_NO_WARNINGS
    )
endif()

# ============================================================================
# POST_BUILD：复制运行时依赖文件
# ============================================================================
if(WIN32 AND EXISTS ${CURL_BIN_DIR}/libcurl-x64.dll)
    add_custom_command(TARGET test_smoke POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CURL_BIN_DIR}/libcurl-x64.dll
            $<TARGET_FILE_DIR:test_smoke>/libcurl-x64.dll
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CURL_BIN_DIR}/curl-ca-bundle.crt
            $<TARGET_FILE_DIR:test_smoke>/curl-ca-bundle.crt
    )
endif()

# ============================================================================
# 注册测试
# ============================================================================
add_test(NAME test_smoke COMMAND test_smoke)
