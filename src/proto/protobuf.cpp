#include "proto/protobuf.hpp"

#include <algorithm>
#include <format>
#include <limits>

namespace x2::proto {

void Writer::varint(std::uint64_t value) {
    while (value >= 0x80) {
        data_.push_back(static_cast<std::uint8_t>(value | 0x80));
        value >>= 7;
    }
    data_.push_back(static_cast<std::uint8_t>(value));
}

void Writer::field_key(std::uint32_t field, std::uint32_t wire_type) {
    varint((static_cast<std::uint64_t>(field) << 3) | wire_type);
}

void Writer::int32(std::uint32_t field, std::int32_t value) {
    field_key(field, 0);
    varint(static_cast<std::uint64_t>(static_cast<std::int64_t>(value)));
}

void Writer::uint32(std::uint32_t field, std::uint32_t value) {
    field_key(field, 0);
    varint(value);
}

void Writer::int64(std::uint32_t field, std::int64_t value) {
    field_key(field, 0);
    varint(static_cast<std::uint64_t>(value));
}

void Writer::uint64(std::uint32_t field, std::uint64_t value) {
    field_key(field, 0);
    varint(value);
}

void Writer::boolean(std::uint32_t field, bool value) {
    field_key(field, 0);
    varint(value ? 1 : 0);
}

void Writer::bytes(std::uint32_t field, std::span<const std::uint8_t> value) {
    field_key(field, 2);
    varint(value.size());
    data_.insert(data_.end(), value.begin(), value.end());
}

void Writer::string(std::uint32_t field, std::string_view value) {
    bytes(field, {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

void Writer::message(std::uint32_t field, std::span<const std::uint8_t> value) {
    bytes(field, value);
}

std::optional<Reader::Field> Reader::field() {
    const auto key = varint();
    if (!key) return std::nullopt;
    const auto number = static_cast<std::uint32_t>(*key >> 3);
    const auto wire_type = static_cast<std::uint32_t>(*key & 7);
    if (number == 0 || wire_type == 3 || wire_type == 4) return std::nullopt;
    return Field{number, wire_type};
}

std::optional<std::uint64_t> Reader::varint() {
    std::uint64_t value{};
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (pos_ >= data_.size()) return std::nullopt;
        const std::uint8_t byte = data_[pos_++];
        value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) return value;
    }
    return std::nullopt;
}

std::optional<std::span<const std::uint8_t>> Reader::bytes() {
    const auto size = varint();
    if (!size || *size > data_.size() || pos_ > data_.size() - *size) return std::nullopt;
    auto result = data_.subspan(pos_, *size);
    pos_ += *size;
    return result;
}

std::optional<std::string> Reader::string() {
    const auto value = bytes();
    if (!value) return std::nullopt;
    return std::string{reinterpret_cast<const char*>(value->data()), value->size()};
}

bool Reader::skip(std::uint32_t wire_type) {
    switch (wire_type) {
        case 0: return varint().has_value();
        case 1:
            if (pos_ + 8 > data_.size()) return false;
            pos_ += 8;
            return true;
        case 2: return bytes().has_value();
        case 5:
            if (pos_ + 4 > data_.size()) return false;
            pos_ += 4;
            return true;
        default: return false;
    }
}

namespace {

std::string hex(std::span<const std::uint8_t> data) {
    std::string out = "0x";
    for (const auto byte : data.first(std::min<std::size_t>(data.size(), 64))) out += std::format("{:02x}", byte);
    if (data.size() > 64) out += "...";
    return out;
}

bool dump_into(std::span<const std::uint8_t> data, std::string& out, int depth) {
    Reader reader{data};
    for (bool first = true; !reader.eof(); first = false) {
        const auto field = reader.field();
        if (!field) return false;
        out += std::format("{}{}:", first ? "" : " ", field->number);
        const auto start = reader.position();
        if (field->wire_type == 0) {
            const auto value = reader.varint();
            if (!value) return false;
            out += std::to_string(static_cast<std::int64_t>(*value));
        } else if (field->wire_type == 2) {
            const auto value = reader.bytes();
            if (!value) return false;
            std::string nested;
            if (std::ranges::all_of(*value, [](std::uint8_t c) { return c >= 0x20 && c != 0x7f; })) {
                out += std::format("\"{}\"", std::string_view{reinterpret_cast<const char*>(value->data()), value->size()});
            } else if (depth < 16 && dump_into(*value, nested, depth + 1)) {
                out += "{" + nested + "}";
            } else {
                out += hex(*value);
            }
        } else {
            if (!reader.skip(field->wire_type)) return false;
            out += hex(data.subspan(start, reader.position() - start));
        }
    }
    return true;
}

} // namespace

std::string dump(std::span<const std::uint8_t> data) {
    std::string out;
    if (!dump_into(data, out, 0)) out += " <malformed>";
    return out;
}

} // namespace x2::proto
