#pragma once

// 可脚本化的本地 HTTP 服务器（仅回环、单连接串行处理）。
//
// 为什么需要它
// ------------
// 现有的 BlockingTcpEndpoint 只能停在 TCP 层：客户端连上后一个字节都不回，
// 覆盖的是"avformat 卡在读上"这一种形态。真实网络源还有一批完全不同的失败面：
//   * 服务端明确回 404（分片 / 文件不存在）；
//   * 回了响应头但 body 迟迟不来（CDN 半死）；
//   * body 只发了一部分就断开（连接被中间设备掐断）；
//   * HLS 清单本身能读到，但引用的分片 404。
// 这些形态用文件或外网地址都没法既确定又离线地复现，只能自己起一个能按路径
// 脚本化回包的 HTTP 服务。
//
// 实现要点
// --------
//   * 只监听 127.0.0.1 的内核分配端口，不依赖外网、不依赖机器装了什么东西；
//   * 单线程串行处理连接：用例里同一时刻只有一个客户端（打开链路 + 随后的
//     诊断扫描都会重开一条连接，串行处理足够，且避免引入连接池的复杂度）；
//   * 支持 Range（206）：FFmpeg 的 http 协议打开 MP4 会 Range 请求，不回 206
//     会被当成不可 seek 的流，MP4 就"打不开"了 —— 那是被测代码的问题，不是
//     服务端脚本的问题，不能让它污染结论；
//   * 停止通过 running_ + 关闭监听 socket 实现：accept 用 50ms 超时的 select，
//     recv 用 SO_RCVTIMEO，stall 阶段每 20ms 复查一次，析构不会卡住；
//   * 响应体里的 "{PORT}" 会替换成实际端口，便于把清单里的分片 URI 指回本服务。

// windows.h 的 min/max 宏会破坏 std::min/std::max（本文件大量使用）。
// 必须先于 BlockingTcpEndpoint.h 里的 winsock2.h/windows.h 生效。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "BlockingTcpEndpoint.h" // 复用 socket 别名与 Windows WSAStartup

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace videoeye_test {

class HttpTestServer {
public:
    enum class Mode {
        ServeRanged,        // 200 / 206，完整 body，支持 Range
        Status,             // 固定状态码，空 body
        StallBeforeHeaders, // 接受连接后一个字节都不发
        StallAfterHeaders,  // 发完响应头后不再发 body，连接保持
        TruncateBody,       // 头声明完整长度，只发 prefix_bytes 后断开
    };

    struct Route {
        std::string path;
        Mode mode = Mode::ServeRanged;
        int status = 200;
        std::string body;
        size_t prefix_bytes = 0;
        std::string content_type = "application/octet-stream";
    };

    static Route BodyRoute(std::string path, std::string body, std::string content_type = "application/octet-stream") {
        return Route{std::move(path), Mode::ServeRanged, 200, std::move(body), 0, std::move(content_type)};
    }

    static Route StatusRoute(std::string path, int status) {
        return Route{std::move(path), Mode::Status, status, std::string(), 0, "text/plain"};
    }

    // stall_before_headers=false 时先发一个声明大 Content-Length 的头，再停住不发 body。
    static Route StallRoute(std::string path, bool stall_before_headers) {
        Route route;
        route.path = std::move(path);
        route.mode = stall_before_headers ? Mode::StallBeforeHeaders : Mode::StallAfterHeaders;
        route.status = 200;
        route.body.assign(64 * 1024, '\0');
        route.content_type = "video/mp4";
        return route;
    }

    static Route TruncateRoute(std::string path, std::string body, size_t prefix_bytes,
                               std::string content_type = "video/mp4") {
        Route route;
        route.path = std::move(path);
        route.mode = Mode::TruncateBody;
        route.status = 200;
        route.body = std::move(body);
        route.prefix_bytes = prefix_bytes;
        route.content_type = std::move(content_type);
        return route;
    }

    explicit HttpTestServer(std::vector<Route> routes) : routes_(std::move(routes)) {
        const videoeye_test_socket_t s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == VIDEOEYE_TEST_INVALID_SOCKET)
            return;

        // 允许析构后立即复用端口（用例之间会连续起多个服务实例）。
        const int reuse = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            videoeye_test_close_socket(s);
            return;
        }
        if (listen(s, 8) != 0) {
            videoeye_test_close_socket(s);
            return;
        }

        sockaddr_in bound{};
#ifdef _WIN32
        int bound_len = sizeof(bound);
#else
        socklen_t bound_len = sizeof(bound);
#endif
        if (getsockname(s, reinterpret_cast<sockaddr*>(&bound), &bound_len) != 0) {
            videoeye_test_close_socket(s);
            return;
        }

        listen_fd_ = s;
        port_ = ntohs(bound.sin_port);
        thread_ = std::thread([this]() { Loop(); });
    }

    ~HttpTestServer() {
        running_.store(false);
        if (listen_fd_ != VIDEOEYE_TEST_INVALID_SOCKET) {
            videoeye_test_close_socket(listen_fd_);
            listen_fd_ = VIDEOEYE_TEST_INVALID_SOCKET;
        }
        if (thread_.joinable())
            thread_.join();
    }

    HttpTestServer(const HttpTestServer&) = delete;
    HttpTestServer& operator=(const HttpTestServer&) = delete;

    bool valid() const {
        return port_ != 0;
    }
    int port() const {
        return port_;
    }

    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

private:
    static std::string StatusText(int status) {
        switch (status) {
        case 200:
            return "OK";
        case 206:
            return "Partial Content";
        case 404:
            return "Not Found";
        case 416:
            return "Range Not Satisfiable";
        default:
            return "Error";
        }
    }

    std::string SubstitutePort(std::string text) const {
        const std::string needle = "{PORT}";
        const std::string replacement = std::to_string(port_);
        size_t pos = 0;
        while ((pos = text.find(needle, pos)) != std::string::npos) {
            text.replace(pos, needle.size(), replacement);
            pos += replacement.size();
        }
        return text;
    }

    void Loop() {
        while (running_.load(std::memory_order_acquire)) {
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(listen_fd_, &read_fds);
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 50 * 1000;
#ifdef _WIN32
            const int ready = select(0, &read_fds, nullptr, nullptr, &tv);
#else
            const int ready = select(listen_fd_ + 1, &read_fds, nullptr, nullptr, &tv);
#endif
            if (!running_.load(std::memory_order_acquire))
                break;
            if (ready <= 0)
                continue;

            const videoeye_test_socket_t conn = accept(listen_fd_, nullptr, nullptr);
            if (conn == VIDEOEYE_TEST_INVALID_SOCKET)
                continue;
            if (running_.load(std::memory_order_acquire))
                HandleConnection(conn);
            videoeye_test_close_socket(conn);
        }
    }

    bool SendAll(videoeye_test_socket_t conn, const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            if (!running_.load(std::memory_order_acquire))
                return false;
            const size_t chunk = std::min<size_t>(data.size() - sent, 16 * 1024);
#ifdef _WIN32
            const int n = send(conn, data.data() + sent, static_cast<int>(chunk), 0);
#else
            const ssize_t n = send(conn, data.data() + sent, chunk, 0);
#endif
            if (n <= 0)
                return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    static std::string ReadRequestHeaders(videoeye_test_socket_t conn, const std::atomic<bool>& running) {
        std::string request;
        char buffer[1024];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192 &&
               running.load(std::memory_order_acquire)) {
#ifdef _WIN32
            const int n = recv(conn, buffer, static_cast<int>(sizeof(buffer)), 0);
#else
            const ssize_t n = recv(conn, buffer, sizeof(buffer), 0);
#endif
            if (n <= 0)
                break;
            request.append(buffer, static_cast<size_t>(n));
        }
        return request;
    }

    static std::string RequestPath(const std::string& request) {
        // 请求行: "GET /clip.mp4 HTTP/1.1"
        const size_t first = request.find(' ');
        if (first == std::string::npos)
            return {};
        const size_t second = request.find(' ', first + 1);
        if (second == std::string::npos)
            return {};
        std::string path = request.substr(first + 1, second - first - 1);
        const size_t query = path.find('?');
        if (query != std::string::npos)
            path.erase(query);
        return path;
    }

    static bool ParseRange(const std::string& request, size_t total, size_t* start, size_t* end) {
        std::string lower = request;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const std::string key = "range: bytes=";
        const size_t pos = lower.find(key);
        if (pos == std::string::npos)
            return false;
        const size_t value_begin = pos + key.size();
        const size_t value_end = lower.find("\r\n", value_begin);
        const std::string value = lower.substr(value_begin, value_end - value_begin);
        const size_t dash = value.find('-');
        if (dash == std::string::npos)
            return false;
        try {
            const size_t first = static_cast<size_t>(std::stoull(value.substr(0, dash)));
            *start = std::min(first, total);
            if (dash + 1 < value.size())
                *end = std::min(static_cast<size_t>(std::stoull(value.substr(dash + 1))), total - 1);
            else
                *end = total == 0 ? 0 : total - 1;
        } catch (...) {
            return false;
        }
        return total > 0;
    }

    void HandleConnection(videoeye_test_socket_t conn) {
        // recv 设超时，stall/服务端停止时才不会永久卡在 read 上。
#ifdef _WIN32
        const int timeout_ms = 100;
        setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 100 * 1000;
        setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif

        const std::string request = ReadRequestHeaders(conn, running_);
        if (request.empty())
            return;
        const std::string path = RequestPath(request);

        const Route* route = nullptr;
        for (const Route& candidate : routes_) {
            if (candidate.path == path) {
                route = &candidate;
                break;
            }
        }

        if (!route) { // 未登记路径一律 404：分片缺失等场景的默认行为
            SendAll(conn, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            return;
        }

        if (route->mode == Mode::StallBeforeHeaders) {
            while (running_.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return;
        }

        std::string body = SubstitutePort(route->body);

        if (route->mode == Mode::Status) {
            const std::string header = "HTTP/1.1 " + std::to_string(route->status) + " " + StatusText(route->status) +
                                       "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            SendAll(conn, header);
            return;
        }

        if (route->mode == Mode::StallAfterHeaders) {
            const std::string header = "HTTP/1.1 200 OK\r\nContent-Type: " + route->content_type +
                                       "\r\nContent-Length: " + std::to_string(body.size()) +
                                       "\r\nConnection: close\r\n\r\n";
            SendAll(conn, header);
            while (running_.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return;
        }

        if (route->mode == Mode::TruncateBody) {
            const std::string header = "HTTP/1.1 200 OK\r\nContent-Type: " + route->content_type +
                                       "\r\nContent-Length: " + std::to_string(body.size()) +
                                       "\r\nConnection: close\r\n\r\n";
            SendAll(conn, header);
            SendAll(conn, body.substr(0, std::min(route->prefix_bytes, body.size())));
            return; // 直接断开：客户端会看到"内容长度不足"
        }

        // ServeRanged: 有 Range 就回 206 切片，否则 200 全量。
        size_t range_start = 0;
        size_t range_end = body.empty() ? 0 : body.size() - 1;
        if (!body.empty() && ParseRange(request, body.size(), &range_start, &range_end)) {
            if (range_start >= body.size()) {
                SendAll(conn, "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" +
                                  std::to_string(body.size()) + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                return;
            }
            const std::string slice = body.substr(range_start, range_end - range_start + 1);
            const std::string header =
                "HTTP/1.1 206 Partial Content\r\nContent-Type: " + route->content_type +
                "\r\nAccept-Ranges: bytes\r\nContent-Range: bytes " + std::to_string(range_start) + "-" +
                std::to_string(range_end) + "/" + std::to_string(body.size()) +
                "\r\nContent-Length: " + std::to_string(slice.size()) + "\r\nConnection: close\r\n\r\n";
            if (SendAll(conn, header))
                SendAll(conn, slice);
            return;
        }

        const std::string header = "HTTP/1.1 200 OK\r\nContent-Type: " + route->content_type +
                                   "\r\nAccept-Ranges: bytes\r\nContent-Length: " + std::to_string(body.size()) +
                                   "\r\nConnection: close\r\n\r\n";
        if (SendAll(conn, header))
            SendAll(conn, body);
    }

    std::vector<Route> routes_;
    std::atomic<bool> running_{true};
    videoeye_test_socket_t listen_fd_ = VIDEOEYE_TEST_INVALID_SOCKET;
    int port_ = 0;
    std::thread thread_;
};

} // namespace videoeye_test
