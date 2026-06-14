// net/src/socket_utils.cpp
#include <net/socket_utils.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace net {

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        throw std::runtime_error("fcntl F_GETFL: " + std::string(std::strerror(errno)));
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
        throw std::runtime_error("fcntl F_SETFL O_NONBLOCK: " + std::string(std::strerror(errno)));
}

void set_reuseaddr(int fd) {
    int yes = 1;
    static_cast<void>(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)));
#ifdef SO_REUSEPORT
    // SO_REUSEPORT allows multiple sockets to bind the same port and avoids
    // EADDRINUSE when the previous socket is still in TIME_WAIT (critical for tests).
    static_cast<void>(setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes)));
#endif
}

void set_tcp_nodelay(int fd) {
    int yes = 1;
    static_cast<void>(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)));
}

int tcp_listen(uint16_t port, int backlog) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        throw std::runtime_error("socket: " + std::string(std::strerror(errno)));

    set_reuseaddr(fd);
    set_nonblocking(fd);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        throw std::runtime_error("bind port " + std::to_string(port) + ": "
                                 + std::strerror(errno));
    }

    if (listen(fd, backlog) < 0) {
        ::close(fd);
        throw std::runtime_error("listen: " + std::string(std::strerror(errno)));
    }

    return fd;
}

int tcp_connect_blocking(const std::string& host, uint16_t port) {
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    int gai = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (gai != 0)
        throw std::runtime_error("getaddrinfo(" + host + "): "
                                 + gai_strerror(gai));

    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        throw std::runtime_error("socket: " + std::string(std::strerror(errno)));
    }

    if (::connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res);
        ::close(fd);
        throw std::runtime_error("connect " + host + ":" + std::to_string(port)
                                 + ": " + std::strerror(errno));
    }

    freeaddrinfo(res);
    set_tcp_nodelay(fd);
    return fd;
}

int tcp_accept(int listen_fd) {
    sockaddr_in peer{};
    socklen_t   len = sizeof(peer);
    int fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &len);
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
        throw std::runtime_error("accept: " + std::string(std::strerror(errno)));
    }
    set_nonblocking(fd);
    set_tcp_nodelay(fd);
    return fd;
}

} // namespace net
