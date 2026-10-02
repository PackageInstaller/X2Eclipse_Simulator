#include "server/marsnet.hpp"

#include <algorithm>
#include <array>
#include <limits>

#include "proto/protobuf.hpp"

namespace x2::server::marsnet {

namespace {

constexpr std::uint32_t kCrcPoly = 0xEDB88320;

std::uint32_t zigzag(std::int32_t value) {
    return (static_cast<std::uint32_t>(value) << 1) ^
           static_cast<std::uint32_t>(value >> 31);
}

std::int32_t unzigzag(std::uint32_t value) {
    return static_cast<std::int32_t>((value >> 1) ^ (0U - (value & 1)));
}

const std::array<std::uint32_t, 256>& crc_table() {
    static const auto table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t i = 0; i < result.size(); ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1) ? (kCrcPoly ^ (value >> 1)) : (value >> 1);
            }
            result[i] = value;
        }
        return result;
    }();
    return table;
}

} // namespace

std::size_t pack_int_size(std::int32_t value) {
    const auto encoded = zigzag(value);
    if (encoded <= 0x7f) return 1;
    if (encoded - 1 <= 0x3ffe) return 2;
    if (encoded - 1 <= 0x1ffffe) return 3;
    if (encoded - 1 <= 0x0ffffffe) return 4;
    return 5;
}

void pack_int(std::vector<std::uint8_t>& output, std::int32_t value) {
    const auto encoded = zigzag(value);
    if (encoded <= 0x7f) {
        output.push_back(static_cast<std::uint8_t>(encoded));
    } else if (encoded - 1 <= 0x3ffe) {
        output.push_back(static_cast<std::uint8_t>((encoded >> 8) | 0x80));
        output.push_back(static_cast<std::uint8_t>(encoded));
    } else if (encoded - 1 <= 0x1ffffe) {
        output.push_back(static_cast<std::uint8_t>((encoded >> 16) | 0xc0));
        output.push_back(static_cast<std::uint8_t>(encoded >> 8));
        output.push_back(static_cast<std::uint8_t>(encoded));
    } else if (encoded - 1 <= 0x0ffffffe) {
        output.push_back(static_cast<std::uint8_t>((encoded >> 24) | 0xe0));
        output.push_back(static_cast<std::uint8_t>(encoded >> 16));
        output.push_back(static_cast<std::uint8_t>(encoded >> 8));
        output.push_back(static_cast<std::uint8_t>(encoded));
    } else {
        output.push_back(0xf0);
        output.push_back(static_cast<std::uint8_t>(encoded >> 24));
        output.push_back(static_cast<std::uint8_t>(encoded >> 16));
        output.push_back(static_cast<std::uint8_t>(encoded >> 8));
        output.push_back(static_cast<std::uint8_t>(encoded));
    }
}

UnpackResult unpack_int(std::span<const std::uint8_t> input) {
    if (input.empty()) return {};
    const std::uint8_t first = input.front();
    std::size_t length;
    std::uint32_t encoded;
    if ((first & 0x80) == 0) {
        length = 1;
        encoded = first;
    } else if ((first & 0x40) == 0) {
        if (input.size() < 2) return {};
        length = 2;
        encoded = (static_cast<std::uint32_t>(first & 0x3f) << 8) | input[1];
    } else if ((first & 0x20) == 0) {
        if (input.size() < 3) return {};
        length = 3;
        encoded = (static_cast<std::uint32_t>(first & 0x1f) << 16) |
                  (static_cast<std::uint32_t>(input[1]) << 8) | input[2];
    } else if ((first & 0x10) == 0) {
        if (input.size() < 4) return {};
        length = 4;
        encoded = (static_cast<std::uint32_t>(first & 0x0f) << 24) |
                  (static_cast<std::uint32_t>(input[1]) << 16) |
                  (static_cast<std::uint32_t>(input[2]) << 8) | input[3];
    } else {
        if (input.size() < 5) return {};
        length = 5;
        encoded = (static_cast<std::uint32_t>(input[1]) << 24) |
                  (static_cast<std::uint32_t>(input[2]) << 16) |
                  (static_cast<std::uint32_t>(input[3]) << 8) | input[4];
    }
    return {unzigzag(encoded), length, true};
}

std::uint32_t crc32(std::span<const std::uint8_t> data) {
    const auto& table = crc_table();
    std::uint32_t crc = 0xffffffff;
    for (const auto byte : data) {
        crc = table[(crc ^ byte) & 0xff] ^ (crc >> 8);
    }
    return crc ^ 0xffffffff;
}

std::vector<std::uint8_t> RequestHead::serialize() const {
    proto::Writer writer;
    writer.uint32(1, request_id);
    if (!session_id.empty()) writer.string(2, session_id);
    writer.int32(3, ack_data_version);
    writer.int64(4, sign);
    writer.int64(5, unit_id);
    return writer.data();
}

std::optional<RequestHead> RequestHead::deserialize(std::span<const std::uint8_t> input) {
    RequestHead result;
    proto::Reader reader{input};
    while (!reader.eof()) {
        const auto field = reader.field();
        if (!field) return std::nullopt;
        switch (field->number) {
            case 1: {
                const auto value = reader.varint();
                if (!value) return std::nullopt;
                result.request_id = static_cast<std::uint32_t>(*value);
                break;
            }
            case 2: {
                const auto value = reader.string();
                if (!value) return std::nullopt;
                result.session_id = std::move(*value);
                break;
            }
            case 3: {
                const auto value = reader.varint();
                if (!value) return std::nullopt;
                result.ack_data_version = static_cast<std::int32_t>(static_cast<std::int64_t>(*value));
                break;
            }
            case 4: {
                const auto value = reader.varint();
                if (!value) return std::nullopt;
                result.sign = static_cast<std::int64_t>(*value);
                break;
            }
            case 5: {
                const auto value = reader.varint();
                if (!value) return std::nullopt;
                result.unit_id = static_cast<std::int64_t>(*value);
                break;
            }
            default:
                if (!reader.skip(field->wire_type)) return std::nullopt;
        }
    }
    return result;
}

std::vector<std::uint8_t> ResponseHead::serialize() const {
    proto::Writer writer;
    writer.uint32(1, request_id);
    if (!session_id.empty()) writer.string(2, session_id);
    writer.int32(3, data_version);
    writer.int32(5, error);
    if (!error_info.empty()) writer.string(6, error_info);
    return writer.data();
}

std::vector<std::uint8_t> encode_response(std::uint32_t proto_id,
                                            std::uint32_t request_id,
                                            std::string_view session_id,
                                            std::int32_t data_version,
                                            std::int32_t error,
                                            std::string_view error_info,
                                            std::span<const std::uint8_t> body) {
    ResponseHead head{request_id, std::string{session_id}, data_version, error, std::string{error_info}};
    const auto head_bytes = head.serialize();
    const std::int32_t total = static_cast<std::int32_t>(
        head_bytes.size() + body.size() +
        pack_int_size(static_cast<std::int32_t>(head_bytes.size())) +
        pack_int_size(static_cast<std::int32_t>(proto_id)));
    std::vector<std::uint8_t> output;
    output.reserve(body.size() + head_bytes.size() + 16);
    pack_int(output, total);
    pack_int(output, static_cast<std::int32_t>(head_bytes.size()));
    output.insert(output.end(), head_bytes.begin(), head_bytes.end());
    pack_int(output, static_cast<std::int32_t>(proto_id));
    output.insert(output.end(), body.begin(), body.end());
    return output;
}

static DecodeResult decode_frame(std::span<const std::uint8_t> input) {
    const auto total = unpack_int(input);
    if (!total.ok) return {.needs_more = true};
    std::size_t offset = total.consumed;
    if (input.size() < offset + static_cast<std::size_t>(total.value)) return {.needs_more = true};
    const std::size_t frame_end = offset + static_cast<std::size_t>(total.value);

    const auto head_length = unpack_int(input.subspan(offset));
    if (!head_length.ok || static_cast<std::size_t>(head_length.value) > frame_end - offset - head_length.consumed) {
        return {};
    }
    offset += head_length.consumed;
    const auto head_bytes = input.subspan(offset, static_cast<std::size_t>(head_length.value));
    auto head = RequestHead::deserialize(head_bytes);
    if (!head) return {};
    offset += head_length.value;

    const auto protocol = unpack_int(input.subspan(offset));
    if (!protocol.ok || offset + protocol.consumed > frame_end) return {};
    offset += protocol.consumed;

    const std::size_t body_size = frame_end - offset;
    Frame frame{std::move(*head), static_cast<std::uint32_t>(protocol.value),
                std::vector<std::uint8_t>(input.begin() + static_cast<std::ptrdiff_t>(offset),
                                          input.begin() + static_cast<std::ptrdiff_t>(frame_end))};
    return {std::move(frame), frame_end, false, true};
}

DecodeResult decode(std::span<const std::uint8_t> input) {
    const auto ticks = static_cast<std::size_t>(
        std::ranges::find_if(input, [](std::uint8_t b) { return b != 0; }) - input.begin());
    auto result = decode_frame(input.subspan(ticks));
    if (result.ok) result.consumed += ticks;
    return result;
}

} // namespace x2::server::marsnet
