#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace x2::server::marsnet {

[[nodiscard]] std::size_t pack_int_size(std::int32_t value);
void pack_int(std::vector<std::uint8_t>& output, std::int32_t value);
struct UnpackResult {
    std::int32_t value{};
    std::size_t consumed{};
    bool ok{};
};
[[nodiscard]] UnpackResult unpack_int(std::span<const std::uint8_t> input);

struct RequestHead {
    std::uint32_t request_id{};
    std::string session_id;
    std::int32_t ack_data_version{};
    std::int64_t sign{};
    std::int64_t unit_id{};

    [[nodiscard]] std::vector<std::uint8_t> serialize() const;
    [[nodiscard]] static std::optional<RequestHead> deserialize(std::span<const std::uint8_t> input);
};

struct ResponseHead {
    std::uint32_t request_id{};
    std::string session_id;
    std::int32_t data_version{};
    std::int32_t error{10};
    std::string error_info;

    [[nodiscard]] std::vector<std::uint8_t> serialize() const;
};

struct Frame {
    RequestHead head;
    std::uint32_t proto_id{};
    std::vector<std::uint8_t> body;
};

[[nodiscard]] std::vector<std::uint8_t> encode_response(std::uint32_t proto_id,
                                                       std::uint32_t request_id,
                                                       std::string_view session_id,
                                                       std::int32_t data_version,
                                                       std::int32_t error,
                                                       std::string_view error_info,
                                                       std::span<const std::uint8_t> body);

struct DecodeResult {
    Frame frame;
    std::size_t consumed{};
    bool needs_more{};
    bool ok{};
};
[[nodiscard]] DecodeResult decode(std::span<const std::uint8_t> input);

[[nodiscard]] std::uint32_t crc32(std::span<const std::uint8_t> data);

} // namespace x2::server::marsnet
