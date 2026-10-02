#include "server/websocket.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <mutex>
#include <thread>
#include <vector>

#include "core/log.hpp"

namespace x2::server::ws {

namespace {
constexpr std::string_view tag{"WEBSOCKET"};
constexpr std::string_view kMagicGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

bool send_all(int socket, std::span<const std::uint8_t> data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto sent = ::send(socket, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
        if (sent <= 0) return false;
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}

bool recv_exact(int socket, std::uint8_t* dst, std::size_t len) {
    std::size_t offset = 0;
    while (offset < len) {
        const auto recvd = ::recv(socket, dst + offset, len - offset, 0);
        if (recvd <= 0) return false;
        offset += static_cast<std::size_t>(recvd);
    }
    return true;
}
} // namespace

std::array<std::uint8_t, 20> sha1(std::span<const std::uint8_t> data) {
    std::uint32_t h0 = 0x67452301;
    std::uint32_t h1 = 0xEFCDAB89;
    std::uint32_t h2 = 0x98BADCFE;
    std::uint32_t h3 = 0x10325476;
    std::uint32_t h4 = 0xC3D2E1F0;

    std::vector<std::uint8_t> msg(data.begin(), data.end());
    const std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8;
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) {
        msg.push_back(0x00);
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<std::uint8_t>((bit_len >> (i * 8)) & 0xFF));
    }

    auto rol = [](std::uint32_t val, std::uint32_t bits) noexcept -> std::uint32_t {
        return (val << bits) | (val >> (32 - bits));
    };

    for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        std::array<std::uint32_t, 80> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(msg[chunk + i * 4]) << 24) |
                   (static_cast<std::uint32_t>(msg[chunk + i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(msg[chunk + i * 4 + 2]) << 8) |
                   (static_cast<std::uint32_t>(msg[chunk + i * 4 + 3]));
        }
        for (std::size_t i = 16; i < 80; ++i) {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        std::uint32_t a = h0;
        std::uint32_t b = h1;
        std::uint32_t c = h2;
        std::uint32_t d = h3;
        std::uint32_t e = h4;

        for (std::size_t i = 0; i < 80; ++i) {
            std::uint32_t f = 0, k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const std::uint32_t temp = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = temp;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::array<std::uint8_t, 20> digest{};
    const std::uint32_t h[5] = {h0, h1, h2, h3, h4};
    for (std::size_t i = 0; i < 5; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>((h[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((h[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((h[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(h[i] & 0xFF);
    }
    return digest;
}

std::string base64_encode(std::span<const std::uint8_t> data) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const std::size_t remaining = data.size() - i;
        const std::uint32_t b0 = data[i];
        const std::uint32_t b1 = remaining > 1 ? data[i + 1] : 0;
        const std::uint32_t b2 = remaining > 2 ? data[i + 2] : 0;
        const std::uint32_t triple = (b0 << 16) | (b1 << 8) | b2;

        out.push_back(kTable[(triple >> 18) & 0x3F]);
        out.push_back(kTable[(triple >> 12) & 0x3F]);
        out.push_back(remaining > 1 ? kTable[(triple >> 6) & 0x3F] : '=');
        out.push_back(remaining > 2 ? kTable[triple & 0x3F] : '=');
    }
    return out;
}

std::string make_websocket_accept(std::string_view sec_key) {
    std::string combined;
    combined.reserve(sec_key.size() + kMagicGuid.size());
    combined.append(sec_key);
    combined.append(kMagicGuid);
    const auto digest = sha1({reinterpret_cast<const std::uint8_t*>(combined.data()), combined.size()});
    return base64_encode(digest);
}

std::vector<std::uint8_t> encode_frame(Opcode opcode, std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> frame;
    frame.reserve(10 + payload.size());
    frame.push_back(0x80 | (static_cast<std::uint8_t>(opcode) & 0x0F));
    if (payload.size() <= 125) {
        frame.push_back(static_cast<std::uint8_t>(payload.size()));
    } else if (payload.size() <= 65535) {
        frame.push_back(126);
        frame.push_back(static_cast<std::uint8_t>((payload.size() >> 8) & 0xFF));
        frame.push_back(static_cast<std::uint8_t>(payload.size() & 0xFF));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<std::uint8_t>((payload.size() >> (i * 8)) & 0xFF));
        }
    }
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

std::vector<std::uint8_t> encode_text_frame(std::string_view text) {
    return encode_frame(Opcode::Text, {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

std::vector<std::uint8_t> encode_pong_frame(std::span<const std::uint8_t> payload) {
    return encode_frame(Opcode::Pong, payload);
}

std::vector<std::uint8_t> encode_close_frame() {
    static constexpr std::uint8_t kClosePayload[] = {0x03, 0xE8}; // 1000 Normal Closure
    return encode_frame(Opcode::Close, kClosePayload);
}

WebSocketSession::WebSocketSession(int socket) : socket_(socket) {}

WebSocketSession::~WebSocketSession() {
    stop();
}

void WebSocketSession::start() {
    std::thread([self = shared_from_this()] {
        self->run();
    }).detach();
}

void WebSocketSession::stop() {
    if (closed_.exchange(true)) return;
    if (socket_ >= 0) {
        ::shutdown(socket_, SHUT_RDWR);
        ::close(socket_);
        socket_ = -1;
    }
}

bool WebSocketSession::send_text(std::string_view text) {
    if (closed_ || socket_ < 0) return false;
    const auto frame = encode_text_frame(text);
    std::lock_guard lock{send_mu_};
    return send_all(socket_, frame);
}

bool WebSocketSession::send_binary(std::span<const std::uint8_t> data) {
    if (closed_ || socket_ < 0) return false;
    const auto frame = encode_frame(Opcode::Binary, data);
    std::lock_guard lock{send_mu_};
    return send_all(socket_, frame);
}

void WebSocketSession::run() {
    x2::core::log_line(x2::core::LogLevel::Info, tag, std::format("session started on fd={}", socket_));

    while (!closed_ && socket_ >= 0) {
        std::array<std::uint8_t, 2> header{};
        if (!recv_exact(socket_, header.data(), header.size())) break;

        const bool fin = (header[0] & 0x80) != 0;
        const auto opcode = static_cast<Opcode>(header[0] & 0x0F);
        const bool masked = (header[1] & 0x80) != 0;
        std::uint64_t payload_len = header[1] & 0x7F;

        if (payload_len == 126) {
            std::array<std::uint8_t, 2> len_bytes{};
            if (!recv_exact(socket_, len_bytes.data(), 2)) break;
            payload_len = (static_cast<std::uint64_t>(len_bytes[0]) << 8) | len_bytes[1];
        } else if (payload_len == 127) {
            std::array<std::uint8_t, 8> len_bytes{};
            if (!recv_exact(socket_, len_bytes.data(), 8)) break;
            payload_len = 0;
            for (int i = 0; i < 8; ++i) {
                payload_len = (payload_len << 8) | len_bytes[i];
            }
        }

        if (payload_len > 10 * 1024 * 1024) { // 10MB safety cap
            x2::core::log_line(x2::core::LogLevel::Warn, tag, "frame payload too large");
            break;
        }

        std::array<std::uint8_t, 4> mask_key{};
        if (masked) {
            if (!recv_exact(socket_, mask_key.data(), 4)) break;
        }

        std::vector<std::uint8_t> payload(payload_len);
        if (payload_len > 0) {
            if (!recv_exact(socket_, payload.data(), payload.size())) break;
            if (masked) {
                for (std::size_t i = 0; i < payload.size(); ++i) {
                    payload[i] ^= mask_key[i % 4];
                }
            }
        }

        switch (opcode) {
            case Opcode::Text: {
                const std::string text(reinterpret_cast<const char*>(payload.data()), payload.size());
                x2::core::log_line(x2::core::LogLevel::Info, tag, std::format("recv text: {}", text));
                break;
            }
            case Opcode::Binary: {
                // Heartbeat from BestHTTP (sends empty byte array or ping bytes)
                // Send pong frame so client resets its heartbeat timeout
                const auto pong = encode_pong_frame(payload);
                std::lock_guard lock{send_mu_};
                send_all(socket_, pong);
                break;
            }
            case Opcode::Ping: {
                const auto pong = encode_pong_frame(payload);
                std::lock_guard lock{send_mu_};
                send_all(socket_, pong);
                break;
            }
            case Opcode::Pong: {
                break;
            }
            case Opcode::Close: {
                const auto close_frame = encode_close_frame();
                std::lock_guard lock{send_mu_};
                send_all(socket_, close_frame);
                closed_ = true;
                break;
            }
            default:
                break;
        }
    }

    x2::core::log_line(x2::core::LogLevel::Info, tag, std::format("session ended on fd={}", socket_));
    WebSocketManager::instance().unregister_session(shared_from_this());
    stop();
}

WebSocketManager& WebSocketManager::instance() {
    static WebSocketManager instance;
    return instance;
}

void WebSocketManager::handle_upgraded_connection(int socket) {
    auto session = std::make_shared<WebSocketSession>(socket);
    register_session(session);
    session->start();
}

void WebSocketManager::register_session(std::shared_ptr<WebSocketSession> session) {
    std::lock_guard lock{mu_};
    sessions_.push_back(std::move(session));
    x2::core::log_line(x2::core::LogLevel::Info, tag, std::format("total sessions: {}", sessions_.size()));
}

void WebSocketManager::unregister_session(const std::shared_ptr<WebSocketSession>& session) {
    std::lock_guard lock{mu_};
    std::erase(sessions_, session);
    x2::core::log_line(x2::core::LogLevel::Info, tag, std::format("total sessions: {}", sessions_.size()));
}

void WebSocketManager::broadcast_text(std::string_view text) {
    std::lock_guard lock{mu_};
    x2::core::log_line(x2::core::LogLevel::Info, tag, std::format("broadcasting to {} clients: {}", sessions_.size(), text));
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (!(*it)->send_text(text)) {
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

void WebSocketManager::broadcast_mail(std::int64_t mail_id) {
    // Exact schema expected by WSCometServiceWrapper::OnMessage -> JsonConvert.DeserializeObject<Response<StreamEventsReply>>
    // and pbs.OnMailEvent: {"MailEvent":1,"Id":<id>}
    const std::string payload = std::format(
        R"({{"error":"","code":0,"data":{{"event":"pbs.OnMailEvent","data":"{{\"MailEvent\":1,\"Id\":{}}}"}}}})",
        mail_id);
    broadcast_text(payload);
}

std::size_t WebSocketManager::active_connections() const {
    std::lock_guard lock{mu_};
    return sessions_.size();
}

} // namespace x2::server::ws
