#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace x2::proto {

class Writer {
public:
    void varint(std::uint64_t value);
    void int32(std::uint32_t field, std::int32_t value);
    void uint32(std::uint32_t field, std::uint32_t value);
    void int64(std::uint32_t field, std::int64_t value);
    void uint64(std::uint32_t field, std::uint64_t value);
    void boolean(std::uint32_t field, bool value);
    void bytes(std::uint32_t field, std::span<const std::uint8_t> value);
    void string(std::uint32_t field, std::string_view value);
    void message(std::uint32_t field, std::span<const std::uint8_t> value);

    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return data_; }
    [[nodiscard]] std::vector<std::uint8_t>& mutable_data() noexcept { return data_; }

private:
    void field_key(std::uint32_t field, std::uint32_t wire_type);
    std::vector<std::uint8_t> data_;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) noexcept : data_(data) {}

    struct Field {
        std::uint32_t number;
        std::uint32_t wire_type;
    };

    [[nodiscard]] std::optional<Field> field();
    [[nodiscard]] std::optional<std::uint64_t> varint();
    [[nodiscard]] std::optional<std::span<const std::uint8_t>> bytes();
    [[nodiscard]] std::optional<std::string> string();
    bool skip(std::uint32_t wire_type);
    [[nodiscard]] bool eof() const noexcept { return pos_ >= data_.size(); }
    [[nodiscard]] std::size_t position() const noexcept { return pos_; }
    [[nodiscard]] std::span<const std::uint8_t> remaining() const noexcept { return data_.subspan(pos_); }

private:
    std::span<const std::uint8_t> data_;
    std::size_t pos_{};
};

// Schema-less one-line dump like `protoc --decode_raw`: `1:10 2:"abc" 3:{1:5}`.
// Printable length-delimited fields are shown as strings, otherwise as a
// nested message when they parse fully, else hex. Undecodable tails end with
// " <malformed>".
[[nodiscard]] std::string dump(std::span<const std::uint8_t> data);

} // namespace x2::proto
