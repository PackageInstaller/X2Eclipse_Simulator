#include <chrono>
#include <csignal>
#include <cstdlib>
#include <format>
#include <print>
#include <string_view>
#include <thread>

#include "runtime/backend.hpp"


int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    std::string_view host_data_path = "offline_backend/data/tables.x2data";
    std::uint16_t http_port = 9999;
    std::uint16_t tcp_port = 10001;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--http-port" && i + 1 < argc) http_port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--tcp-port" && i + 1 < argc) tcp_port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--data" && i + 1 < argc) host_data_path = argv[++i];
    }

    x2::runtime::start_from_file(host_data_path, http_port, tcp_port);
    std::println("[MAIN] X2Eclipse offline backend (host debug)");

    std::signal(SIGINT, [](int) { std::exit(0); });
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
    }
    return 0;
}
