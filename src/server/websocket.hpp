#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace x2::server::ws {

enum class Opcode : std::uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

struct Frame {
    bool fin{true};
    Opcode opcode{Opcode::Text};
    std::vector<std::uint8_t> payload;
};

// SHA-1 (RFC 3174) & Base64
[[nodiscard]] std::array<std::uint8_t, 20> sha1(std::span<const std::uint8_t> data);
[[nodiscard]] std::string base64_encode(std::span<const std::uint8_t> data);
[[nodiscard]] std::string make_websocket_accept(std::string_view sec_key);

// RFC 6455 Frame encoding & decoding
[[nodiscard]] std::vector<std::uint8_t> encode_frame(Opcode opcode, std::span<const std::uint8_t> payload);
[[nodiscard]] std::vector<std::uint8_t> encode_text_frame(std::string_view text);
[[nodiscard]] std::vector<std::uint8_t> encode_pong_frame(std::span<const std::uint8_t> payload);
[[nodiscard]] std::vector<std::uint8_t> encode_close_frame();

class WebSocketSession : public std::enable_shared_from_this<WebSocketSession> {
public:
    explicit WebSocketSession(int socket);
    ~WebSocketSession();

    void start();
    void stop();
    bool send_text(std::string_view text);
    bool send_binary(std::span<const std::uint8_t> data);

private:
    void run();

    int socket_{-1};
    std::mutex send_mu_;
    std::atomic<bool> closed_{false};
};

class WebSocketManager {
public:
    static WebSocketManager& instance();

    void handle_upgraded_connection(int socket);
    void register_session(std::shared_ptr<WebSocketSession> session);
    void unregister_session(const std::shared_ptr<WebSocketSession>& session);

    void broadcast_text(std::string_view text);
    void broadcast_mail(std::int64_t mail_id);

    [[nodiscard]] std::size_t active_connections() const;

private:
    WebSocketManager() = default;

    mutable std::mutex mu_;
    std::vector<std::shared_ptr<WebSocketSession>> sessions_;
};

} // namespace x2::server::ws
