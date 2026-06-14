// server/src/main.cpp
//
// Job Manager Server — entry point
//
// Usage:
//   ./job_manager_server [--port 7777] [--db jobs.db]
//
// Graceful shutdown on SIGINT / SIGTERM.

#include <server/server.hpp>
#include <storage/sqlite_backend.hpp>
#include <common/logger.hpp>

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <atomic>

static std::atomic<bool> g_stop{false};
static Server*           g_server_ptr = nullptr;

static void sighandler(int /*sig*/) {
    g_stop = true;
    if (g_server_ptr) g_server_ptr->stop();
}

static std::string get_arg(int argc, char** argv,
                            const std::string& flag, const std::string& def) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == flag) return argv[i + 1];
    }
    return def;
}

int main(int argc, char** argv) {
    Logger::set_level(LogLevel::INFO);

    const std::string db_path  = get_arg(argc, argv, "--db",   "phase2.db");
    const uint16_t    port     = static_cast<uint16_t>(
                                     std::stoi(get_arg(argc, argv, "--port", "7777")));

    std::signal(SIGINT,  sighandler);
    std::signal(SIGTERM, sighandler);

    std::cout << "\033[1;34m"
              << "╔═══════════════════════════════════════╗\n"
              << "║   Distributed Job Queue — Phase 2     ║\n"
              << "║   Job Manager Server                  ║\n"
              << "╚═══════════════════════════════════════╝\n"
              << "\033[0m"
              << "  Port  : " << port    << "\n"
              << "  DB    : " << db_path << "\n\n";

    try {
        auto storage = std::make_unique<SQLiteBackend>(db_path);
        Server server(port, std::move(storage));
        g_server_ptr = &server;

        server.run();  // blocks until SIGINT
    } catch (const std::exception& ex) {
        Logger::error("main", std::string("Fatal: ") + ex.what());
        return EXIT_FAILURE;
    }

    std::cout << "\n\033[1;32m✔ Server shut down cleanly.\033[0m\n";
    return EXIT_SUCCESS;
}
