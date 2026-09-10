// ============================================================================
// CdpBrowserSession — Chrome DevTools Protocol 远程浏览器会话实现
//
// 依赖:
//   - libcurl v8.20+ (curl_ws_send / curl_ws_recv / CURLOPT_CONNECT_ONLY)
//   - Windows: CreateProcessW / TerminateProcessW (kernel32)
//   - Linux:   fork+exec / kill
// ============================================================================

#include "github_research/browser_session_cdp.hpp"
#include "github_research/browser_config.hpp"

#include <curl/curl.h>
#include <curl/websockets.h>
#include <nlohmann/json.hpp>

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <cstring>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#ifndef SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#endif
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

namespace github_research {

using json = nlohmann::json;

// ============================================================================
// 调试日志 (简单 stderr 输出, 与 WebViewSession 风格一致)
// ============================================================================
static void cdp_log(const char* tag, const std::string& msg) {
    std::cerr << "[cdp][" << tag << "] " << msg << std::endl;
}

// ============================================================================
// 工具: 字符串分割
// ============================================================================
static std::vector<std::string> split_args(const std::string& s) {
    std::vector<std::string> tokens;
    std::string cur;
    bool inQuote = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '"') { inQuote = !inQuote; continue; }
        if (!inQuote && (c == ' ' || c == '\t')) {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

// ============================================================================
// 启动 Chrome 子进程
// ============================================================================
bool CdpBrowserSession::launch_chrome(const std::string& cmdlineUtf8) {
#ifdef _WIN32
    // Windows: CreateProcessW 需要完整命令行 (程序+参数) + wstring (UTF-16)
    // argsUtf8 本身就是完整命令行 (build_chrome_command 已拼好 chrome.exe + args)
    int wlen = MultiByteToWideChar(CP_UTF8, 0, cmdlineUtf8.c_str(), -1, nullptr, 0);
    std::wstring wcmd(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, cmdlineUtf8.c_str(), -1, &wcmd[0], wlen);

    cdp_log("init", "CreateProcessW cmdline: " + cmdlineUtf8);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    // 不传 STARTF_USESHOWWINDOW / SW_HIDE — Chrome headless=new 自己处理
    // CREATE_NO_WINDOW 会让某些 Chrome 版本启动后立即退出, 改成普通创建
    memset(&proc_info_, 0, sizeof(proc_info_));

    BOOL ok = CreateProcessW(
        nullptr,                     // lpApplicationName (用命令行中的第一个可执行)
        const_cast<LPWSTR>(wcmd.c_str()),  // lpCommandLine
        nullptr,                     // lpProcessAttributes
        nullptr,                     // lpThreadAttributes
        FALSE,                       // bInheritHandles
        0,                           // dwCreationFlags: 不用 CREATE_NO_WINDOW (Chrome 会崩)
        nullptr,                     // lpEnvironment
        nullptr,                     // lpCurrentDirectory
        &si,
        &proc_info_
    );

    if (!ok) {
        DWORD err = GetLastError();
        cdp_log("init", "CreateProcessW failed, error=" + std::to_string(err));
        return false;
    }

    process_handle_ = proc_info_.hProcess;
    pid_ = static_cast<uint64_t>(proc_info_.dwProcessId);
    CloseHandle(proc_info_.hThread);  // 不需要线程句柄

    cdp_log("init", "Chrome launched, PID=" + std::to_string(pid_));
    return true;

#else
    // Linux/macOS: fork + exec
    pid_t pid = fork();
    if (pid < 0) {
        cdp_log("init", "fork failed");
        return false;
    }
    if (pid == 0) {
        // 子进程: 拆完整命令行 + execvp
        auto argv = split_args(cmdlineUtf8);
        std::vector<char*> charp;
        for (auto& s : argv) charp.push_back(const_cast<char*>(s.c_str()));
        charp.push_back(nullptr);
        execvp(charp[0], charp.data());
        _exit(127);  // exec 失败
    }
    pid_ = static_cast<uint64_t>(pid);
    cdp_log("init", "Chrome launched, PID=" + std::to_string(pid_));
    return true;
#endif
}

// ============================================================================
// 终止 Chrome 进程
// ============================================================================
void CdpBrowserSession::terminate_chrome() {
#ifdef _WIN32
    if (process_handle_) {
        if (TerminateProcess(process_handle_, 0)) {
            WaitForSingleObject(process_handle_, 3000);
        }
        CloseHandle(process_handle_);
        process_handle_ = nullptr;
    }
    memset(&proc_info_, 0, sizeof(proc_info_));
#else
    if (pid_ > 0) {
        kill(static_cast<pid_t>(pid_), SIGTERM);
        // 3 秒内没退再 SIGKILL
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            int status;
            if (waitpid(static_cast<pid_t>(pid_), &status, WNOHANG) != 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        kill(static_cast<pid_t>(pid_), SIGKILL);
        waitpid(static_cast<pid_t>(pid_), nullptr, 0);
    }
#endif
    pid_ = 0;
}

// ============================================================================
// HTTP GET 辅助 (用于 /json/version 和 /json)
// 用 curl easy, 简单一次请求
// ============================================================================
static std::string http_get(const std::string& url, uint32_t timeoutMs) {
    CURL* h = curl_easy_init();
    if (!h) return "";

    std::string body;
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMs));
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, +[](void* d, size_t n, size_t m, void* ptr) -> size_t {
        static_cast<std::string*>(ptr)->append(static_cast<char*>(d), n * m);
        return n * m;
    });
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "identity");  // 避免 gzip 编码干扰
    curl_easy_setopt(h, CURLOPT_NOPROXY, "127.0.0.1,localhost,::1");  // CDP 走 loopback,不能被代理劫持

    CURLcode res = curl_easy_perform(h);
    curl_easy_cleanup(h);
    if (res != CURLE_OK) return "";
    return body;
}

// ============================================================================
// 等待 CDP 端口就绪
// ============================================================================
bool CdpBrowserSession::wait_cdp_ready(uint16_t port, uint32_t timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::string url = "http://127.0.0.1:" + std::to_string(port) + "/json/version";

    while (std::chrono::steady_clock::now() < deadline) {
        std::string body = http_get(url, 2000);
        if (!body.empty()) {
            try {
                json j = json::parse(body);
                if (j.contains("Browser") || j.contains("Protocol-Version")) {
                    cdp_log("init", "CDP ready on port " + std::to_string(port));
                    return true;
                }
            } catch (...) {}
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    cdp_log("init", "CDP port " + std::to_string(port) + " not ready within " + std::to_string(timeoutMs) + "ms");
    return false;
}

// ============================================================================
// 取第一个可用页面的 WebSocket URL
// ============================================================================
std::string CdpBrowserSession::get_page_ws_url(uint16_t port) {
    std::string url = "http://127.0.0.1:" + std::to_string(port) + "/json";
    auto body = http_get(url, 3000);
    if (body.empty()) return "";
    try {
        json arr = json::parse(body);
        if (!arr.is_array() || arr.empty()) return "";
        for (auto& item : arr) {
            if (item.value("type", "") == "page" && item.contains("webSocketDebuggerUrl")) {
                return item["webSocketDebuggerUrl"].get<std::string>();
            }
        }
        // 兜底: 取第一个有 webSocketDebuggerUrl 的
        for (auto& item : arr) {
            if (item.contains("webSocketDebuggerUrl")) {
                return item["webSocketDebuggerUrl"].get<std::string>();
            }
        }
    } catch (...) {}
    return "";
}

// ============================================================================
// 原生 Winsock2 WebSocket (绕开 curl WS 高层 API, 避免 curl_ws_send 对
// Chrome CDP loopback 的 "Failed sending data" 错误)
// 流程: TCP connect → 手动发 HTTP WebSocket upgrade → 收到 101 →
//       之后 send()/recv() + 手写 WS 帧编解码
// ============================================================================

#ifdef _WIN32
static bool ws_set_recv_timeout(SOCKET sock, uint32_t timeoutMs) {
    DWORD tv = timeoutMs;
    return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv)) == 0;
}
static ssize_t ws_raw_send(SOCKET s, const char* buf, size_t len) {
    return send(s, buf, (int)len, 0);
}
static ssize_t ws_raw_recv(SOCKET s, char* buf, size_t len) {
    return recv(s, buf, (int)len, 0);
}
#else
static bool ws_set_recv_timeout(int sock, uint32_t timeoutMs) {
    struct timeval tv = {(long)(timeoutMs/1000), (long)((timeoutMs%1000)*1000)};
    return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
}
static ssize_t ws_raw_send(int s, const char* buf, size_t len) {
    return ::send(s, buf, len, 0);
}
static ssize_t ws_raw_recv(int s, char* buf, size_t len) {
    return ::recv(s, buf, len, 0);
}
#endif

// 生成随机 16-byte key + base64, 用于 Sec-WebSocket-Key
static std::string ws_make_key_b64() {
    unsigned char key[16];
    // 简单随机, CDP Chrome 不严格校验, 只要 base64 能解出 16 bytes
    for (int i = 0; i < 16; i++) key[i] = (unsigned char)(rand() & 0xFF);
    // base64 编码 (标准 MIME, 无换行)
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(24);
    for (int i = 0; i < 16; i += 3) {
        uint32_t n = ((uint32_t)key[i] << 16) |
                     (i+1 < 16 ? ((uint32_t)key[i+1] << 8) : 0) |
                     (i+2 < 16 ?  (uint32_t)key[i+2]       : 0);
        out += b64[(n >> 18) & 0x3F];
        out += b64[(n >> 12) & 0x3F];
        out += (i+1 < 16) ? b64[(n >> 6) & 0x3F] : '=';
        out += (i+2 < 16) ? b64[n & 0x3F]       : '=';
    }
    return out;
}

// WS frame 发送: opcode=2 (binary), 带客户端必须的 MASK
template<typename SockT>
static bool ws_send_binary_frame(SockT sock, const char* data, size_t len) {
    // 构造 frame header
    std::vector<char> header;
    header.push_back(0x81);  // FIN=1, opcode=1 (text — CDP 只接受文本 JSON)
    if (len < 126) {
        header.push_back((char)(0x80 | len));          // MASK=1 + payload_len
    } else if (len < 65536) {
        header.push_back(0xFE);                        // MASK=1 + 126=2byte ext
        header.push_back((char)((len >> 8) & 0xFF));
        header.push_back((char)( len        & 0xFF));
    } else {
        header.push_back(0xFF);                        // MASK=1 + 127=8byte ext
        for (int i = 7; i >= 0; i--) header.push_back((char)((len >> (i*8)) & 0xFF));
    }
    // 随机 mask key
    unsigned char mask[4] = {(unsigned char)(rand()&0xFF),(unsigned char)(rand()&0xFF),
                              (unsigned char)(rand()&0xFF),(unsigned char)(rand()&0xFF)};
    header.push_back((char)mask[0]);
    header.push_back((char)mask[1]);
    header.push_back((char)mask[2]);
    header.push_back((char)mask[3]);

    // 先发 header (循环直到全部发完, TCP 可能部分发送)
    size_t hSent = 0;
    while (hSent < header.size()) {
        ssize_t n = ws_raw_send(sock, header.data() + hSent, header.size() - hSent);
        if (n <= 0) {
#ifdef _WIN32
            cdp_log("ws", "ws_send header failed, WSAErr=" + std::to_string(WSAGetLastError())
                   + " hSent=" + std::to_string(hSent));
#else
            cdp_log("ws", std::string("ws_send header failed errno=") + strerror(errno));
#endif
            return false;
        }
        hSent += n;
    }

    // 再发 masked payload (小块, 避免 send 截断)
    std::vector<char> masked(len);
    for (size_t i = 0; i < len; i++) masked[i] = data[i] ^ mask[i & 3];
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ws_raw_send(sock, masked.data() + sent, len - sent);
        if (n <= 0) return false;
        sent += n;
    }
    // hex dump header for debug
    std::string hx;
    for (auto b : header) { char buf[4]; sprintf_s(buf, "%02X ", (unsigned char)b); hx += buf; }
    cdp_log("ws", std::string("SEND frame hdr[") + std::to_string(hSent) + "]=" + hx
           + " payload_len=" + std::to_string(len));
    return true;
}

// WS frame 接收: 返回 payload (自动处理 opcodes, 跳过 ping/pong/close)
// 设置 SO_RCVTIMEO 后, recv 超时返回 ""
template<typename SockT>
static std::string ws_recv_any_frame(SockT sock) {
    // 分片拼接缓冲 (CDP 用短消息, Chrome 很少分片, 但 Page.navigate 响应 > 125 字节可能分片)
    static std::string frag_buf;
    static int frag_opcode = -1;
    static bool frag_active = false;

    // 读 header: 至少 2 bytes
    char hdr[10];
    ssize_t n = ws_raw_recv(sock, hdr, 2);
    if (n <= 0) return "";
    while (n < 2) {
        ssize_t r = ws_raw_recv(sock, hdr + n, 2 - n);
        if (r <= 0) return "";
        n += r;
    }

    bool    fin     = (hdr[0] & 0x80) != 0;
    uint8_t opcode  = hdr[0] & 0x0F;
    bool    masked  = (hdr[1] & 0x80) != 0;
    uint64_t plen   = hdr[1] & 0x7F;
    size_t  hdr_off = 2;

    if (plen == 126) {
        n = ws_raw_recv(sock, hdr + hdr_off, 2);
        if (n <= 0) return "";
        plen = ((uint8_t)hdr[hdr_off] << 8) | (uint8_t)hdr[hdr_off+1];
        hdr_off += 2;
    } else if (plen == 127) {
        plen = 0;
        for (int i = 0; i < 8; i++) {
            n = ws_raw_recv(sock, hdr + hdr_off, 1);
            if (n <= 0) return "";
            plen = (plen << 8) | (uint8_t)hdr[hdr_off];
            hdr_off++;
        }
    }

    uint8_t mask[4] = {0,0,0,0};
    if (masked) {
        for (int i = 0; i < 4; i++) {
            n = ws_raw_recv(sock, (char*)mask + i, 1);
            if (n <= 0) return "";
        }
    }

    // 读 payload
    std::string payload(plen, '\0');
    if (plen > 0) {
        size_t got = 0;
        while (got < plen) {
            n = ws_raw_recv(sock, &payload[got], plen - got);
            if (n <= 0) return "";
            got += n;
        }
        if (masked) for (size_t i = 0; i < plen; i++) payload[i] ^= mask[i & 3];
    }

    // 控制帧 (可以在分片中间穿插): ping→pong, close→return ""
    if (opcode >= 0x08) {
        if (opcode == 0x09) {
            std::vector<char> pong_hdr;
            pong_hdr.push_back((char)0x8A);
            pong_hdr.push_back((char)(plen & 0x7F));
            ws_raw_send(sock, pong_hdr.data(), pong_hdr.size());
            if (plen > 0) ws_raw_send(sock, payload.data(), payload.size());
            return ws_recv_any_frame(sock);
        }
        if (opcode == 0x08) {
            frag_buf.clear(); frag_opcode = -1; frag_active = false;
            return "";
        }
    }

    // 分片处理
    // opcode=1/2 数据帧: FIN=0 开始分片, FIN=1 单片直接返回
    // opcode=0 continuation: 追加到缓冲, FIN=1 结束返回完整
    if (opcode == 0x00) {
        // continuation
        if (!frag_active) return "";  // 异常 continuation
        frag_buf += payload;
        if (fin) {
            std::string full = std::move(frag_buf);
            frag_buf.clear(); frag_opcode = -1; frag_active = false;
            // debug
            {
                std::string preview = full.substr(0, 200);
                cdp_log("ws", std::string("RECV defragmented opcode=") + std::to_string(frag_opcode)
                       + " len=" + std::to_string(full.size())
                       + " payload=" + preview);
            }
            return full;
        } else {
            return "";  // 还没到最后一片
        }
    }

    // opcode=1 (text) 或 opcode=2 (binary)
    if (opcode == 0x01 || opcode == 0x02) {
        if (!fin) {
            // 开始分片
            frag_buf = std::move(payload);
            frag_opcode = opcode;
            frag_active = true;
            cdp_log("ws", std::string("RECV frag start opcode=") + std::to_string(opcode)
                   + " len=" + std::to_string(plen));
            return "";
        } else {
            // 单片完整帧 — 同时强制落盘到文件, 绕过日志截断
            FILE* f = fopen("D:\\DeerFlow\\DeerFlow++\\ws_frames.log", "a");
            if (f) {
                fprintf(f, "=== RECV opcode=%d fin=1 len=%llu ===\n", opcode, (unsigned long long)plen);
                // hex dump
                for (uint64_t i = 0; i < plen; i++) {
                    fprintf(f, "%02X ", (unsigned char)payload[i]);
                    if ((i+1) % 16 == 0) fprintf(f, "\n");
                }
                fprintf(f, "\nASCII: ");
                for (uint64_t i = 0; i < plen; i++) {
                    char c = payload[i];
                    fprintf(f, "%c", (c >= 32 && c < 127) ? c : '.');
                }
                fprintf(f, "\n\n");
                fclose(f);
            }
            cdp_log("ws", std::string("RECV opcode=") + std::to_string(opcode)
                   + " fin=1 len=" + std::to_string(plen));
            return payload;
        }
    }

    // 其他未知 opcode
    return "";
}

bool CdpBrowserSession::connect_websocket(const std::string& wsUrl) {
    // 1. 解析 wsUrl: ws://127.0.0.1:9222/devtools/page/xxx
    //    host=127.0.0.1, port=9222, path=/devtools/page/xxx
    std::string url = wsUrl;
    if (url.find("ws://") == 0) url = url.substr(5);
    else if (url.find("wss://") == 0) url = url.substr(6);
    size_t slash = url.find('/');
    std::string hostPort = (slash == std::string::npos) ? url : url.substr(0, slash);
    std::string path = (slash == std::string::npos) ? "/" : url.substr(slash);

    size_t colon = hostPort.rfind(':');
    std::string host = (colon == std::string::npos) ? hostPort : hostPort.substr(0, colon);
    uint16_t port = (colon == std::string::npos) ? 80 : (uint16_t)atoi(hostPort.substr(colon+1).c_str());

    // 2. 初始化 Winsock (Windows only)
#ifdef _WIN32
    static bool wsa_init_done = false;
    if (!wsa_init_done) {
        WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
        wsa_init_done = true;
    }
#endif

    // 3. TCP connect
#ifdef _WIN32
    ws_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ws_sock_ == INVALID_SOCKET) { cdp_log("ws", "socket() failed"); return false; }
#else
    ws_sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (ws_sock_ < 0) { cdp_log("ws", "socket() failed"); return false; }
#endif

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    // connect 带简单超时 (非阻塞 + select)
    std::string keyB64 = ws_make_key_b64();

    auto startT = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - startT < std::chrono::seconds(5)) {
#ifdef _WIN32
        if (connect(ws_sock_, (sockaddr*)&addr, sizeof(addr)) == 0) break;
        if (WSAGetLastError() != WSAEINPROGRESS) {
            closesocket(ws_sock_); ws_sock_ = INVALID_SOCKET;
            cdp_log("ws", std::string("connect failed: ") + std::to_string(WSAGetLastError()));
            return false;
        }
        fd_set wfds; FD_ZERO(&wfds); FD_SET(ws_sock_, &wfds);
        struct timeval tv = {0, 100*1000};  // 100ms
        select(0, nullptr, &wfds, nullptr, &tv);
#else
        if (::connect(ws_sock_, (sockaddr*)&addr, sizeof(addr)) == 0) break;
        if (errno != EINPROGRESS) {
            ::close(ws_sock_); ws_sock_ = -1;
            cdp_log("ws", std::string("connect failed: ") + strerror(errno));
            return false;
        }
        fd_set wfds; FD_ZERO(&wfds); FD_SET(ws_sock_, &wfds);
        struct timeval tv = {0, 100*1000};
        select(ws_sock_+1, nullptr, &wfds, nullptr, &tv);
#endif
    }

    // 4. 发 HTTP WebSocket upgrade 请求
    std::string req =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: " + host + ":" + std::to_string(port) + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + keyB64 + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    ssize_t n = ws_raw_send(ws_sock_, req.data(), req.size());
    if (n != (ssize_t)req.size()) {
        cdp_log("ws", "upgrade request send incomplete");
        goto fail;
    }

    // 5. 读响应, 验证 101 Switching Protocols
    {
        char rbuf[4096];
        std::string resp;
        ws_set_recv_timeout(ws_sock_, 5000);
        size_t headerEnd = std::string::npos;
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (headerEnd == std::string::npos &&
               std::chrono::steady_clock::now() < dl) {
            n = ws_raw_recv(ws_sock_, rbuf, sizeof(rbuf));
            if (n > 0) {
                resp.append(rbuf, n);
                headerEnd = resp.find("\r\n\r\n");
            }
        }
        if (resp.find("101 Switching Protocols") == std::string::npos &&
            resp.find("101 ") == std::string::npos) {
            cdp_log("ws", std::string("WS upgrade failed, response starts with: ") +
                    resp.substr(0, std::min(resp.size(), (size_t)200)));
            goto fail;
        }
    }

    // 6. 改回短超时 (send/recv 循环里自己管总超时)
    ws_set_recv_timeout(ws_sock_, 100);  // 100ms 短 poll

    cdp_log("ws", "WebSocket connected: " + wsUrl);
    return true;

fail:
#ifdef _WIN32
    if (ws_sock_ != INVALID_SOCKET) { closesocket(ws_sock_); ws_sock_ = INVALID_SOCKET; }
#else
    if (ws_sock_ >= 0) { ::close(ws_sock_); ws_sock_ = -1; }
#endif
    return false;
}

void CdpBrowserSession::close_websocket() {
    std::lock_guard<std::mutex> lk(ws_mutex_);
#ifdef _WIN32
    if (ws_sock_ != INVALID_SOCKET) {
        // 发 close frame (unmasked server→client... 不对, 这里我们是客户端, 必须 mask)
        // 简单处理: 直接关 socket
        closesocket(ws_sock_);
        ws_sock_ = INVALID_SOCKET;
    }
#else
    if (ws_sock_ >= 0) {
        ::close(ws_sock_);
        ws_sock_ = -1;
    }
#endif
}

// ============================================================================
// 发送 CDP 命令 + 等待匹配响应
// ============================================================================
std::string CdpBrowserSession::send_cdp_command(const std::string& method,
                                               const std::string& paramsJson,
                                               uint32_t timeoutMs) {
#ifdef _WIN32
    if (ws_sock_ == INVALID_SOCKET) return "";
#else
    if (ws_sock_ < 0) return "";
#endif

    int myId = ++next_msg_id_;

    // 构造 CDP 请求体: {"id":N, "method":"...", "params":{...}}
    json req;
    req["id"] = myId;
    req["method"] = method;
    if (!paramsJson.empty()) {
        try {
            req["params"] = json::parse(paramsJson);
        } catch (...) {
            req["params"] = json::object();
        }
    } else {
        req["params"] = json::object();
    }
    std::string payload = req.dump();

    // === 发送 ===
    {
        std::lock_guard<std::mutex> lk(ws_mutex_);
        if (!ws_send_binary_frame(ws_sock_, payload.data(), payload.size())) {
            cdp_log("cdp", "ws_send_binary_frame failed");
            return "";
        }
    }

    // === 接收循环 ===
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    while (std::chrono::steady_clock::now() < deadline) {
        std::string frame;
        {
            std::lock_guard<std::mutex> lk(ws_mutex_);
            frame = ws_recv_any_frame(ws_sock_);
        }

        if (frame.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        // 解析响应
        try {
            json msg = json::parse(frame);

            // 事件消息 (没有 id): 跳过继续等待
            if (!msg.contains("id")) {
                continue;
            }

            // 匹配 id
            if (msg["id"].get<int>() == myId) {
                return frame;
            }
            // id 不匹配, 继续等待
            cdp_log("cdp", std::string("recv frame id=") + std::to_string(msg["id"].get<int>())
                   + " expected=" + std::to_string(myId) + " payload="
                   + frame.substr(0, 150));
        } catch (const std::exception& e) {
            cdp_log("cdp", std::string("json parse error: ") + e.what()
                   + " frame=" + frame.substr(0, 150));
        } catch (...) {
        }
    }

    cdp_log("cdp", "Command timeout for method=" + method + " id=" + std::to_string(myId));
    return "";
}

bool CdpBrowserSession::send_browser_close() {
    std::string resp = send_cdp_command("Browser.close", "", 3000);
    return !resp.empty();
}

// ============================================================================
// Constructor / Destructor
// ============================================================================
CdpBrowserSession::CdpBrowserSession() = default;

CdpBrowserSession::~CdpBrowserSession() {
    Destroy();
}

// ============================================================================
// Init
// ============================================================================
bool CdpBrowserSession::Init(const std::string& userDataDir,
                            const std::string& extraArgs,
                            const std::string& proxy_url) {
    if (ready_) return true;

    // 确保 BrowserConfig 已加载 (幂等, 已加载过不重复)
    static std::once_flag cfg_loaded;
    std::call_once(cfg_loaded, [] { BrowserConfig::instance().load_defaults(); });

    auto& cfg = BrowserConfig::instance();

    user_data_dir_ = userDataDir;
    proxy_url_ = proxy_url;

    // 1. 探测 Chrome 路径 (委托 BrowserConfig)
    chrome_path_ = cfg.resolve_chrome_path();
    if (chrome_path_.empty()) {
        cdp_log("init", "Chrome binary not found. Searched standard paths + CHROME_PATH.");
        return false;
    }
    cdp_log("init", "Using Chrome: " + chrome_path_);

    // 2. 分配调试端口 (从配置起始端口 + 原子递增, 多会话安全)
    static std::atomic<uint16_t> port_counter{0};
    if (port_counter.load() == 0) {
        // 第一次调用: 基配置起始端口
        debug_port_ = static_cast<uint16_t>(cfg.cdp.debug_port_range_start);
        port_counter.store(debug_port_ + 1);
    } else {
        debug_port_ = port_counter.fetch_add(1);
    }

    // 3. 构造启动命令 (委托 BrowserConfig)
    std::string cmdline = cfg.build_chrome_command(chrome_path_, debug_port_, userDataDir, proxy_url, extraArgs);
    cdp_log("init", "Command: " + cmdline);

    // 4. 启动 Chrome
    if (!launch_chrome(cmdline)) {
        cdp_log("init", "launch_chrome failed");
        return false;
    }

    // 5. 等待 CDP 端口就绪 (超时从配置读取)
    if (!wait_cdp_ready(debug_port_, static_cast<uint32_t>(cfg.cdp.connect_timeout_ms))) {
        terminate_chrome();
        return false;
    }

    // 6. 取 WebSocket URL
    std::string wsUrl = get_page_ws_url(debug_port_);
    if (wsUrl.empty()) {
        cdp_log("init", "Failed to get page WebSocket URL from /json");
        terminate_chrome();
        return false;
    }
    cdp_log("init", "Page wsUrl: " + wsUrl);

    // 7. 建立 WebSocket 连接
    if (!connect_websocket(wsUrl)) {
        terminate_chrome();
        return false;
    }

    // 8. 启用 Page + Runtime 域
    send_cdp_command("Page.enable", "{}", 5000);
    send_cdp_command("Runtime.enable", "{}", 5000);

    ready_ = true;
    cdp_log("init", "CDP session ready");
    return true;
}

// ============================================================================
// Destroy
// ============================================================================
void CdpBrowserSession::Destroy() {
    if (destroyed_.exchange(true)) return;

    cdp_log("destroy", "Destroying CDP session");
    ready_ = false;

    // 1. 尝试 Browser.close 优雅关闭
#ifdef _WIN32
    if (ws_sock_ != INVALID_SOCKET) {
#else
    if (ws_sock_ >= 0) {
#endif
        send_browser_close();
        close_websocket();
    }

    // 2. 强杀 Chrome 进程
    terminate_chrome();

    cdp_log("destroy", "CDP session destroyed");
}

// ============================================================================
// Navigate
// ============================================================================
bool CdpBrowserSession::Navigate(const std::string& url) {
    if (!ready_) return false;

    json params;
    params["url"] = url;
    std::string resp = send_cdp_command("Page.navigate", params.dump(), 10000);
    if (resp.empty()) return false;

    // navigate 的响应只返回 frameId, 真正的加载结果需要等事件
    try {
        json j = json::parse(resp);
        if (j.contains("error")) {
            cdp_log("navigate", "CDP error: " + j["error"].dump());
            return false;
        }
    } catch (...) {}

    return true;
}

// ============================================================================
// WaitForNavigation — 轮询等 Page.loadEventFired / Page.frameStoppedLoading
// ============================================================================
bool CdpBrowserSession::WaitForNavigation(uint32_t timeoutMs) {
    if (!ready_) return false;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    while (std::chrono::steady_clock::now() < deadline) {
        std::string msg;
        {
            std::lock_guard<std::mutex> lk(ws_mutex_);
            msg = ws_recv_any_frame(ws_sock_);
        }

        if (msg.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        try {
            json j = json::parse(msg);
            std::string method = j.value("method", "");

            if (method == "Page.loadEventFired") {
                cdp_log("nav", "Navigation completed (loadEventFired)");
                return true;
            }
            if (method == "Page.frameStoppedLoading") {
                cdp_log("nav", "Navigation completed (frameStoppedLoading)");
                return true;
            }
            // Page.domContentEventFired 也算加载完成
            if (method == "Page.domContentEventFired") {
                // DOM ready, 再等一下 loadEventFired
                // 不返回, 继续等
            }
        } catch (...) {
            // JSON 不完整, 继续
        }
    }

    cdp_log("nav", "WaitForNavigation timeout");
    return false;
}

// ============================================================================
// ExecuteScript — Runtime.evaluate
// ============================================================================
ScriptResult CdpBrowserSession::ExecuteScript(const std::string& jsCode,
                                              uint32_t timeoutMs) {
    ScriptResult result;
    if (!ready_) {
        result.error = "browser not ready";
        return result;
    }

    json params;
    params["expression"] = jsCode;
    params["returnByValue"] = true;
    params["awaitPromise"] = true;

    std::string resp = send_cdp_command("Runtime.evaluate", params.dump(), timeoutMs);
    if (resp.empty()) {
        result.error = "Runtime.evaluate timeout or connection error";
        return result;
    }

    try {
        json j = json::parse(resp);

        // CDP error
        if (j.contains("error")) {
            result.error = j["error"].value("message", "unknown CDP error");
            return result;
        }

        if (!j.contains("result") || !j["result"].contains("result")) {
            result.error = "malformed CDP response: no result.result field";
            return result;
        }

        json evaluateResult = j["result"]["result"];

        // 检查 exception
        if (j["result"].contains("exceptionDetails") && !j["result"]["exceptionDetails"].is_null()) {
            result.error = "JS exception: " + j["result"]["exceptionDetails"].dump();
            return result;
        }

        // 提取 value
        std::string type = evaluateResult.value("type", "");
        if (evaluateResult.contains("value")) {
            json val = evaluateResult["value"];
            result.data = val.dump();  // 原样返回 JSON 序列化
            result.success = true;
        } else if (type == "undefined") {
            result.data = "null";
            result.success = true;
        } else {
            result.error = "unsupported result type: " + type;
            return result;
        }

        return result;

    } catch (const std::exception& e) {
        result.error = std::string("JSON parse failed: ") + e.what();
        return result;
    }
}

// ============================================================================
// CheckLogin
// ============================================================================
bool CdpBrowserSession::CheckLogin(const std::string& loginDetectJs) {
    ScriptResult sr = ExecuteScript(loginDetectJs, 5000);
    if (!sr.success) return false;
    // 简单判定: data 包含 "true" 或 "success":true
    return sr.data.find("true") != std::string::npos
        || sr.data.find("success\":true") != std::string::npos;
}

} // namespace github_research
