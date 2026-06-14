// client_lib/src/client_main.cpp
//
// CLI job submission client
//
// Usage:
//   ./job_client --host 127.0.0.1 --port 7777
//               --type sleep_job --payload "200"
//               [--priority HIGH|NORMAL|LOW] [--retries 3]

#include <client/job_client.hpp>
#include <common/logger.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

static std::string get_arg(int argc, char** argv,
                            const std::string& flag, const std::string& def) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == flag) return argv[i + 1];
    }
    return def;
}

int main(int argc, char** argv) {
    Logger::set_level(LogLevel::WARN);  // suppress noise for CLI usage

    const std::string host       = get_arg(argc, argv, "--host",     "127.0.0.1");
    const uint16_t    port       = static_cast<uint16_t>(
                                       std::stoi(get_arg(argc, argv, "--port", "7777")));
    const std::string job_type   = get_arg(argc, argv, "--type",     "sleep_job");
    const std::string payload    = get_arg(argc, argv, "--payload",  "100");
    const std::string prio_str   = get_arg(argc, argv, "--priority", "NORMAL");
    const int         max_retries = std::stoi(get_arg(argc, argv, "--retries", "3"));

    Priority priority = priority_from_string(prio_str);

    JobClient client(host, port);
    auto result = client.submit(job_type, payload, priority, max_retries);

    if (result.ok()) {
        std::cout << "Submitted: " << result.value() << "\n";
        return EXIT_SUCCESS;
    } else {
        std::cerr << "Error: " << result.error() << "\n";
        return EXIT_FAILURE;
    }
}
