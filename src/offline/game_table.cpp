#include "offline/game_table.hpp"

#include <cstring>
#include <utility>

#include "proto/protobuf.hpp"

namespace x2::offline {

bool GameTable::load(const TableBlob& blob, std::string_view name) {
    const TableBlob::Entry* entry = blob.find(name);
    if (!entry) return false;
    owned_.assign(entry->data.begin(), entry->data.end());
    raw_ = owned_;
    name_ = std::string{name};
    rows_.clear();

    if (raw_.size() < 4) return false;
    const auto capacity = static_cast<std::uint32_t>(raw_[0]) |
                          (static_cast<std::uint32_t>(raw_[1]) << 8) |
                          (static_cast<std::uint32_t>(raw_[2]) << 16) |
                          (static_cast<std::uint32_t>(raw_[3]) << 24);
    rows_.reserve(capacity);

    proto::Reader reader{raw_.subspan(4)};
    std::vector<std::uint64_t> keys;
    std::vector<std::span<const std::uint8_t>> items;
    while (!reader.eof()) {
        const auto field = reader.field();
        if (!field) break;
        if (field->number == 1 && field->wire_type == 0) {
            const auto key = reader.varint();
            if (!key) break;
            keys.push_back(*key);
        } else if (field->number == 2 && field->wire_type == 2) {
            const auto item = reader.bytes();
            if (!item) break;
            items.push_back(*item);
        } else if (!reader.skip(field->wire_type)) {
            break;
        }
    }
    const std::size_t count = keys.size() < items.size() ? keys.size() : items.size();
    rows_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        rows_.push_back(TableRow{keys[i], items[i]});
    }
    return !rows_.empty();
}

std::optional<TableRow> GameTable::row(std::uint64_t key) const noexcept {
    for (const auto& item : rows_) {
        if (item.key == key) return item;
    }
    return std::nullopt;
}

std::optional<std::int32_t> GameTable::int_field(const TableRow& row, std::uint32_t field) const {
    proto::Reader reader{row.bytes};
    while (!reader.eof()) {
        const auto current = reader.field();
        if (!current) return std::nullopt;
        if (current->number == field && current->wire_type == 0) {
            const auto value = reader.varint();
            if (!value) return std::nullopt;
            return static_cast<std::int32_t>(static_cast<std::int64_t>(*value));
        }
        if (!reader.skip(current->wire_type)) return std::nullopt;
    }
    return std::nullopt;
}

std::vector<std::int32_t> GameTable::ints_field(const TableRow& row, std::uint32_t field) const {
    std::vector<std::int32_t> out;
    proto::Reader reader{row.bytes};
    while (!reader.eof()) {
        const auto current = reader.field();
        if (!current) break;
        if (current->number == field && current->wire_type == 0) {
            out.push_back(static_cast<std::int32_t>(static_cast<std::int64_t>(reader.varint().value_or(0))));
        } else if (current->number == field && current->wire_type == 2) { // proto3 packed
            proto::Reader packed{reader.bytes().value_or(std::span<const std::uint8_t>{})};
            while (!packed.eof()) {
                const auto value = packed.varint();
                if (!value) break;
                out.push_back(static_cast<std::int32_t>(static_cast<std::int64_t>(*value)));
            }
        } else if (!reader.skip(current->wire_type)) {
            break;
        }
    }
    return out;
}

std::optional<std::string> GameTable::string_field(const TableRow& row, std::uint32_t field) const {
    proto::Reader reader{row.bytes};
    while (!reader.eof()) {
        const auto current = reader.field();
        if (!current) return std::nullopt;
        if (current->number == field && current->wire_type == 2) {
            return reader.string();
        }
        if (!reader.skip(current->wire_type)) return std::nullopt;
    }
    return std::nullopt;
}

} // namespace x2::offline
