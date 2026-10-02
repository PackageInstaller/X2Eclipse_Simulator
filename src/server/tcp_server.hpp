#pragma once

#include <cstdint>
#include <string_view>

#include "server/router.hpp"

namespace x2::server {

class MarsNetTcpServer {
public:
    explicit MarsNetTcpServer(Router& router) : router_(router) {}
    bool listen(std::string_view host, std::uint16_t port);
    void run();

private:
    void handle_client(int client);

    Router& router_;
    int server_fd_{-1};
};

} // namespace x2::server
