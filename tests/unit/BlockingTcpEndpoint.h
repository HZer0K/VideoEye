#pragma once

// 一个"只监听、从不 accept、从不回包"的本地 TCP 端口。
//
// 用途: 给单测提供一个**可控的阻塞 IO 输入**。连上来的客户端（FFmpeg 的 tcp 协议）
// 会一直停在"等数据"上 —— 不依赖外网、不需要大文件、也不依赖机器上装了什么，
// 却能稳定复现"avformat_open_input / av_read_frame 长时间不返回"的场景。
// 这正是"取消必须能打断阻塞 IO"这类结论唯一能被自动化的验证方式：
// 拿一个真实的大文件或外网地址都做不到既确定又离线。
//
// 实现要点:
//   * 监听 socket 上的连接由内核完成三次握手（backlog 内不需要 accept），
//     所以客户端 connect 会成功，随后 recv() 永久阻塞 —— 没人会发任何字节。
//   * 用 tcp:// 而不是 http://: FFmpeg 的 http 协议会读 http_proxy 环境变量，
//     本机常见的 Clash 之类的代理会把请求改道，本地端口就永远等不到连接。

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
using videoeye_test_socket_t = SOCKET;
#  define VIDEOEYE_TEST_INVALID_SOCKET INVALID_SOCKET
#  define videoeye_test_close_socket closesocket
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
using videoeye_test_socket_t = int;
#  define VIDEOEYE_TEST_INVALID_SOCKET (-1)
#  define videoeye_test_close_socket ::close
#endif

#include <string>

namespace videoeye_test {

class BlockingTcpEndpoint {
public:
    BlockingTcpEndpoint() {
#ifdef _WIN32
        static const bool wsa_ready = [] {
            WSADATA data;
            return WSAStartup(MAKEWORD(2, 2), &data) == 0;
        }();
        if (!wsa_ready) return;
#endif
        const videoeye_test_socket_t s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == VIDEOEYE_TEST_INVALID_SOCKET) return;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // 让内核挑一个空闲端口
        if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            videoeye_test_close_socket(s);
            return;
        }
        if (listen(s, 4) != 0) {
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

        fd_ = s;
        port_ = ntohs(bound.sin_port);
    }

    ~BlockingTcpEndpoint() {
        if (fd_ != VIDEOEYE_TEST_INVALID_SOCKET) videoeye_test_close_socket(fd_);
    }

    BlockingTcpEndpoint(const BlockingTcpEndpoint&) = delete;
    BlockingTcpEndpoint& operator=(const BlockingTcpEndpoint&) = delete;

    bool valid() const { return fd_ != VIDEOEYE_TEST_INVALID_SOCKET; }
    int port() const { return port_; }

    std::string tcp_url() const {
        return "tcp://127.0.0.1:" + std::to_string(port_);
    }

private:
    videoeye_test_socket_t fd_ = VIDEOEYE_TEST_INVALID_SOCKET;
    int port_ = 0;
};

}  // namespace videoeye_test
