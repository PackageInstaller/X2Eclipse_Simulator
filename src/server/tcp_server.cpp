#include "server/tcp_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <format>
#include <thread>
#include <vector>

#include "core/log.hpp"
#include "server/marsnet.hpp"

namespace x2::server {

namespace {
constexpr std::string_view tag{"MARSNET"};

bool send_all(int socket, std::span<const std::uint8_t> data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto sent = ::send(socket, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
        if (sent <= 0) return false;
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}
}

bool MarsNetTcpServer::listen(std::string_view host, std::uint16_t port) {
    server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        x2::core::log_error_errno(tag, "socket");
        return false;
    }
    int yes = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = host.empty() || host == "0.0.0.0" ? INADDR_ANY : inet_addr(host.data());
    if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        ::listen(server_fd_, 16) < 0) {
        x2::core::log_error_errno(tag, "bind/listen");
        return false;
    }
    return true;
}

void MarsNetTcpServer::run() {
    while (true) {
        sockaddr_in peer{};
        socklen_t peer_size = sizeof(peer);
        const int client = ::accept(server_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_size);
        if (client < 0) {
            x2::core::log_error_errno(tag, "accept");
            continue;
        }
        std::thread([this, client] { handle_client(client); }).detach();
    }
}

void MarsNetTcpServer::handle_client(int client) {
    std::vector<std::uint8_t> buffer;
    std::array<std::uint8_t, 65536> input{};
    x2::core::log_line(x2::core::LogLevel::Info, tag, "client connected");
    while (true) {
        const auto received = ::recv(client, input.data(), input.size(), 0);
        if (received <= 0) break;
        // A lone 0x00 byte is the MarsNet client heartbeat tick; echo it back
        // so the client sees the peer as alive, and keep it out of the frame
        // decoder buffer. Timed college builds complete on this beat.
        if (received == 1 && input[0] == 0x00 && buffer.empty()) {
            if (!send_all(client, {&input[0], 1})) break;
            const auto pushes = router_.poll_pushes();
            if (!pushes.empty() && !send_all(client, pushes)) {
                x2::core::log_line(x2::core::LogLevel::Error, tag, "send failed (push)");
                ::close(client);
                return;
            }
            continue;
        }
        {
            std::string hex;
            for (std::size_t i = 0; i < static_cast<std::size_t>(received) && i < 96; ++i) {
                hex += std::format("{:02x}", input[i]);
                if ((i & 31) == 31) hex += "\n         ";
            }
            x2::core::log_line(x2::core::LogLevel::Info, tag,
                               std::format("recv {} bytes: {}", received, hex));
        }
        buffer.insert(buffer.end(), input.begin(), input.begin() + received);
        while (true) {
            const auto decoded = marsnet::decode(buffer);
            if (!decoded.ok || decoded.needs_more) break;
            const auto response = router_.marsnet(decoded.frame);
            if (!send_all(client, response)) {
                x2::core::log_line(x2::core::LogLevel::Error, tag, "send failed");
                ::close(client);
                return;
            }
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(decoded.consumed));
        }
    }
    ::close(client);
    x2::core::log_line(x2::core::LogLevel::Info, tag, "client disconnected");
}

} // namespace x2::server
