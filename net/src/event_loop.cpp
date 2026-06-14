// net/src/event_loop.cpp
//
// Platform-conditional I/O multiplexing:
//   Linux  → epoll (level-triggered)
//   macOS  → kqueue
//
// Level-triggered is used on Linux (no EPOLLET) to match kqueue's default
// semantics. Both guarantee: if data is available, you keep getting notified.

#include <net/event_loop.hpp>

#include <common/logger.hpp>

#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// ─── Platform includes ────────────────────────────────────────────────────────

#if defined(__linux__)
#  include <sys/epoll.h>
#elif defined(__APPLE__) || defined(__FreeBSD__)
#  include <sys/event.h>
#  include <sys/time.h>
#else
#  error "Unsupported platform: only Linux and macOS are supported."
#endif

namespace net {

// ─── Constructor / Destructor ─────────────────────────────────────────────────

EventLoop::EventLoop() {
#if defined(__linux__)
    poll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (poll_fd_ < 0)
        throw std::runtime_error("epoll_create1: " + std::string(std::strerror(errno)));
#else
    poll_fd_ = kqueue();
    if (poll_fd_ < 0)
        throw std::runtime_error("kqueue: " + std::string(std::strerror(errno)));
#endif
    Logger::debug("EventLoop", "Initialized (poll_fd=" + std::to_string(poll_fd_) + ")");
}

EventLoop::~EventLoop() {
    if (poll_fd_ >= 0) ::close(poll_fd_);
}

// ─── Public interface (delegates to platform_*) ───────────────────────────────

void EventLoop::add_fd(int fd, uint32_t events, EventCallback cb) {
    callbacks_[fd] = std::move(cb);
    platform_add(fd, events);
}

void EventLoop::modify_fd(int fd, uint32_t events) {
    platform_modify(fd, events);
}

void EventLoop::remove_fd(int fd) {
    platform_remove(fd);
    callbacks_.erase(fd);
}

void EventLoop::poll_once(int timeout_ms) {
    platform_wait(timeout_ms);
}

void EventLoop::run() {
    running_ = true;
    while (running_) {
        platform_wait(100);  // 100 ms ceiling so stop() is detected quickly
    }
}

void EventLoop::stop() {
    running_ = false;
}

// ═════════════════════════════════════════════════════════════════════════════
// Linux — epoll (level-triggered)
// ═════════════════════════════════════════════════════════════════════════════

#if defined(__linux__)

void EventLoop::platform_add(int fd, uint32_t events) {
    epoll_event ev{};
    ev.data.fd = fd;
    if (events & Events::READ)  ev.events |= EPOLLIN;
    if (events & Events::WRITE) ev.events |= EPOLLOUT;
    // Level-triggered by default (no EPOLLET).
    if (epoll_ctl(poll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0)
        Logger::warn("EventLoop", "epoll_ctl ADD fd=" + std::to_string(fd)
                                   + ": " + std::strerror(errno));
}

void EventLoop::platform_modify(int fd, uint32_t events) {
    epoll_event ev{};
    ev.data.fd = fd;
    if (events & Events::READ)  ev.events |= EPOLLIN;
    if (events & Events::WRITE) ev.events |= EPOLLOUT;
    if (epoll_ctl(poll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0)
        Logger::warn("EventLoop", "epoll_ctl MOD fd=" + std::to_string(fd)
                                   + ": " + std::strerror(errno));
}

void EventLoop::platform_remove(int fd) {
    // On Linux < 2.6.9 the fourth argument must be non-NULL; pass a dummy.
    epoll_event ev{};
    epoll_ctl(poll_fd_, EPOLL_CTL_DEL, fd, &ev);
}

void EventLoop::platform_wait(int timeout_ms) {
    static constexpr int kMaxEvents = 64;
    epoll_event evs[kMaxEvents];
    int n = epoll_wait(poll_fd_, evs, kMaxEvents, timeout_ms);
    if (n < 0) {
        if (errno == EINTR) return;
        Logger::error("EventLoop", "epoll_wait: " + std::string(std::strerror(errno)));
        return;
    }

    // Copy ready fds to a local list so that callbacks can safely add/remove fds.
    std::vector<std::pair<int, uint32_t>> ready;
    ready.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        int      fd  = evs[i].data.fd;
        uint32_t ev  = 0u;
        if (evs[i].events & EPOLLIN)  ev |= Events::READ;
        if (evs[i].events & EPOLLOUT) ev |= Events::WRITE;
        if (evs[i].events & EPOLLHUP) ev |= Events::HUP;
        if (evs[i].events & EPOLLERR) ev |= Events::ERR;
        ready.emplace_back(fd, ev);
    }
    for (auto& [fd, ev] : ready) {
        auto it = callbacks_.find(fd);
        if (it != callbacks_.end()) it->second(fd, ev);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// macOS / BSD — kqueue
// ═════════════════════════════════════════════════════════════════════════════

#else  // __APPLE__ / BSD

void EventLoop::platform_add(int fd, uint32_t events) {
    struct kevent changes[2];
    int n = 0;
    if (events & Events::READ)
        EV_SET(&changes[n++], static_cast<uintptr_t>(fd),
               EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    if (events & Events::WRITE)
        EV_SET(&changes[n++], static_cast<uintptr_t>(fd),
               EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    if (n > 0) kevent(poll_fd_, changes, n, nullptr, 0, nullptr);
}

void EventLoop::platform_modify(int fd, uint32_t events) {
    struct kevent changes[4];
    int n = 0;
    auto ufd = static_cast<uintptr_t>(fd);
    if (events & Events::READ)
        EV_SET(&changes[n++], ufd, EVFILT_READ,  EV_ADD | EV_ENABLE, 0, 0, nullptr);
    else
        EV_SET(&changes[n++], ufd, EVFILT_READ,  EV_DELETE, 0, 0, nullptr);
    if (events & Events::WRITE)
        EV_SET(&changes[n++], ufd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    else
        EV_SET(&changes[n++], ufd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    kevent(poll_fd_, changes, n, nullptr, 0, nullptr);
}

void EventLoop::platform_remove(int fd) {
    struct kevent changes[2];
    auto ufd = static_cast<uintptr_t>(fd);
    EV_SET(&changes[0], ufd, EVFILT_READ,  EV_DELETE, 0, 0, nullptr);
    EV_SET(&changes[1], ufd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    kevent(poll_fd_, changes, 2, nullptr, 0, nullptr);
}

void EventLoop::platform_wait(int timeout_ms) {
    static constexpr int kMaxEvents = 64;
    struct kevent evs[kMaxEvents];

    struct timespec ts{};
    struct timespec* ts_ptr = nullptr;
    if (timeout_ms >= 0) {
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = static_cast<long>((timeout_ms % 1000) * 1'000'000L);
        ts_ptr = &ts;
    }

    int n = kevent(poll_fd_, nullptr, 0, evs, kMaxEvents, ts_ptr);
    if (n < 0) {
        if (errno == EINTR) return;
        Logger::error("EventLoop", "kevent: " + std::string(std::strerror(errno)));
        return;
    }

    std::vector<std::pair<int, uint32_t>> ready;
    ready.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        int      fd = static_cast<int>(evs[i].ident);
        uint32_t ev = 0u;
        if (evs[i].filter == EVFILT_READ)  {
            ev |= Events::READ;
            if (evs[i].flags & EV_EOF) ev |= Events::HUP;
        }
        if (evs[i].filter == EVFILT_WRITE) ev |= Events::WRITE;
        if (evs[i].flags  & EV_ERROR)      ev |= Events::ERR;
        ready.emplace_back(fd, ev);
    }
    for (auto& [fd, ev] : ready) {
        auto it = callbacks_.find(fd);
        if (it != callbacks_.end()) it->second(fd, ev);
    }
}

#endif // platform

} // namespace net
