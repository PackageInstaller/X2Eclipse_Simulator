#include "server/http.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <format>
#include <charconv>
#include <print>
#include <thread>

#include "core/log.hpp"
#include "server/websocket.hpp"

namespace x2::server {

namespace {
constexpr std::string_view tag{"HTTP"};

std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\r')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\r')) value.remove_suffix(1);
    return value;
}

bool equals_ci(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

bool send_all(int socket, std::span<const std::uint8_t> data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto sent = ::send(socket, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
        if (sent <= 0) return false;
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}

void write_response(int socket, const HttpResponse& response, bool keep_alive) {
    std::string status_text = response.status == 200 ? "OK" : "Status";
    std::string head = std::format(
        "HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nConnection: {}\r\n\r\n",
        response.status, status_text, response.content_type, response.body.size(),
        keep_alive ? "keep-alive" : "close");
    send_all(socket, {reinterpret_cast<const std::uint8_t*>(head.data()), head.size()});
    send_all(socket, {reinterpret_cast<const std::uint8_t*>(response.body.data()), response.body.size()});
}

void graceful_close(int socket) {
    ::shutdown(socket, SHUT_WR);
    std::array<std::uint8_t, 4096> drain{};
    while (::recv(socket, drain.data(), drain.size(), 0) > 0) {
    }
    ::close(socket);
}

std::string url_decode(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            unsigned decoded = 0;
            const std::string_view hex{value.substr(i + 1, 2)};
            if (std::from_chars(hex.data(), hex.data() + hex.size(), decoded, 16).ec == std::errc{}) {
                result.push_back(static_cast<char>(decoded));
                i += 2;
                continue;
            }
        }
        result.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    return result;
}

void parse_query(std::string_view query, std::map<std::string, std::string>& out) {
    while (!query.empty()) {
        const auto pair_end = query.find('&');
        const auto pair = query.substr(0, pair_end);
        if (!pair.empty()) {
            const auto eq = pair.find('=');
            if (eq == std::string_view::npos) {
                out[url_decode(pair)] = "";
            } else {
                out[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
            }
        }
        query.remove_prefix(pair_end == std::string_view::npos ? query.size() : pair_end + 1);
    }
}
} // namespace

bool HttpServer::listen(std::string_view host, std::uint16_t port) {
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
    if (host.empty() || host == "0.0.0.0") {
        address.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, host.data(), &address.sin_addr) != 1) {
        x2::core::log_line(x2::core::LogLevel::Error, tag, std::format("invalid host {}", host));
        return false;
    }
    if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        ::listen(server_fd_, 16) < 0) {
        x2::core::log_error_errno(tag, "bind/listen");
        return false;
    }
    return true;
}

void HttpServer::run() {
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

void HttpServer::route(std::string path, Handler handler) {
    routes_[std::move(path)] = std::move(handler);
}

void HttpServer::set_default(Handler handler) {
    default_ = std::move(handler);
}

void HttpServer::handle_client(int client) {
    std::array<std::uint8_t, 16384> buffer{};
    for (int served = 0; served < 1024; ++served) {
        std::string request_text;
        std::size_t head_end = std::string::npos;
        std::size_t content_length = 0;
        while (true) {
            const auto received = ::recv(client, buffer.data(), buffer.size(), 0);
            if (received <= 0) return; // peer closed
            request_text.append(reinterpret_cast<const char*>(buffer.data()), static_cast<std::size_t>(received));
            if (head_end == std::string::npos) {
                head_end = request_text.find("\r\n\r\n");
                if (head_end != std::string::npos) {
                    if (request_text.find("100-continue") != std::string::npos &&
                        request_text.starts_with("POST")) {
                        static constexpr std::string_view cont{"HTTP/1.1 100 Continue\r\n\r\n"};
                        send_all(client, {reinterpret_cast<const std::uint8_t*>(cont.data()), cont.size()});
                    }
                    std::string lower = request_text.substr(0, head_end);
                    std::transform(lower.begin(), lower.end(), lower.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    for (std::size_t pos = 0; (pos = lower.find("content-length:", pos)) != std::string::npos;
                         pos += 15) {
                        std::size_t digits = pos + 15;
                        while (digits < lower.size() && lower[digits] == ' ') ++digits;
                        std::size_t value = 0;
                        const auto [ptr, ec] = std::from_chars(lower.data() + digits, lower.data() + lower.size(), value);
                        if (ec == std::errc{}) content_length = std::max(content_length, value);
                    }
                }
            }
            if (head_end != std::string::npos &&
                request_text.size() >= head_end + 4 + content_length) {
                break;
            }
            if (request_text.size() > 1024 * 1024) break;
        }

        if (head_end == std::string::npos) {
            ::close(client);
            return;
        }
        HttpRequest request;
        const std::string head = request_text.substr(0, head_end);
        request.body = request_text.substr(head_end + 4);
        const auto request_line_end = head.find("\r\n");
        const std::string request_line = head.substr(0, request_line_end);
        const auto method_end = request_line.find(' ');
        const auto target_end = request_line.find(' ', method_end + 1);
        if (method_end == std::string::npos || target_end == std::string::npos) {
            write_response(client, {.status = 400, .body = "{\"code\":400}"}, false);
            graceful_close(client);
            return;
        }
        request.method = request_line.substr(0, method_end);
        request.target = request_line.substr(method_end + 1, target_end - method_end - 1);
        const auto query_pos = request.target.find('?');
        request.path = query_pos == std::string::npos ? request.target : request.target.substr(0, query_pos);
        if (query_pos != std::string::npos) parse_query(request.target.substr(query_pos + 1), request.query);

        std::size_t headers_pos = request_line_end + 2;
        while (headers_pos < head.size()) {
            const auto line_end = head.find("\r\n", headers_pos);
            const auto line = head.substr(headers_pos, line_end == std::string::npos ? std::string::npos : line_end - headers_pos);
            const auto colon = line.find(':');
            if (colon != std::string::npos) {
                request.headers[std::string{trim(line.substr(0, colon))}] = std::string{trim(line.substr(colon + 1))};
            }
            if (line_end == std::string::npos) break;
            headers_pos = line_end + 2;
        }

        bool is_ws_upgrade = false;
        std::string sec_ws_key;
        for (const auto& [name, val] : request.headers) {
            if (equals_ci(name, "Upgrade") && equals_ci(val, "websocket")) {
                is_ws_upgrade = true;
            } else if (equals_ci(name, "Sec-WebSocket-Key")) {
                sec_ws_key = val;
            }
        }

        if (is_ws_upgrade && !sec_ws_key.empty()) {
            const std::string accept_key = ws::make_websocket_accept(sec_ws_key);
            const std::string handshake = std::format(
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: {}\r\n\r\n",
                accept_key);
            send_all(client, {reinterpret_cast<const std::uint8_t*>(handshake.data()), handshake.size()});
            x2::core::log_line(x2::core::LogLevel::Info, tag, std::format("WebSocket upgrade 101 on fd={}", client));
            ws::WebSocketManager::instance().handle_upgraded_connection(client);
            return;
        }

        auto handler = routes_.find(request.path);
        HttpResponse response = handler != routes_.end() ? handler->second(request) :
            default_ ? default_(request) : HttpResponse{.body = "{\"code\":0}"};
        x2::core::log_line(x2::core::LogLevel::Info, tag, std::format("{} {} -> {} bytes", request.method, request.path, response.body.size()));

        const bool keep_alive = response.keep_alive &&
            [&] {
                const auto it = request.headers.find("Connection");
                return it == request.headers.end() || !equals_ci(it->second, "close");
            }();
        write_response(client, response, keep_alive);
        if (!keep_alive) {
            graceful_close(client);
            return;
        }
    }
    graceful_close(client);
}

} // namespace x2::server
