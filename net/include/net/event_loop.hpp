#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// event_loop.hpp — I/O event demultiplexer
//
// Abstracts epoll (Linux) and kqueue (macOS) behind a single API.
// All registered callbacks are invoked on the thread that calls run() or poll_once().
//
// Event flag constants:
//   Events::READ  — fd is readable (data available or connection arrived)
//   Events::WRITE — fd is writable (send buffer has space)
//   Events::HUP   — peer closed the connection
//   Events::ERR   — error on fd
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <atomic>

namespace net {

// Normalized event flags (identical meaning on Linux and macOS).
namespace Events {
    static constexpr uint32_t READ  = 0x01u;
    static constexpr uint32_t WRITE = 0x02u;
    static constexpr uint32_t HUP   = 0x04u;
    static constexpr uint32_t ERR   = 0x08u;
} // namespace Events

using EventCallback = std::function<void(int fd, uint32_t events)>;

class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&)            = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // Register fd for the given events. cb is called when events fire.
    // fd must not already be registered.
    void add_fd(int fd, uint32_t events, EventCallback cb);

    // Change which events trigger cb for an already-registered fd.
    void modify_fd(int fd, uint32_t events);

    // Unregister fd. Safe to call even if fd is not registered.
    void remove_fd(int fd);

    // Wait for events and dispatch callbacks.
    // timeout_ms == -1 → block indefinitely.
    void poll_once(int timeout_ms = -1);

    // Run poll_once() in a loop until stop() is called.
    void run();

    // Request the run() loop to exit. Thread-safe.
    void stop();

    [[nodiscard]] bool running() const noexcept { return running_; }

private:
    int  poll_fd_{-1};  // epoll fd (Linux) or kqueue fd (macOS)
    std::atomic<bool> running_{false};
    std::unordered_map<int, EventCallback> callbacks_;

    void platform_add(int fd, uint32_t events);
    void platform_modify(int fd, uint32_t events);
    void platform_remove(int fd);
    // Blocks for up to timeout_ms; fires callbacks for ready events.
    void platform_wait(int timeout_ms);
};

} // namespace net
