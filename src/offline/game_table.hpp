#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "offline/table_blob.hpp"

namespace x2::offline {


struct TableRow {
    std::uint64_t key{};
    std::span<const std::uint8_t> bytes;
};

class GameTable {
public:
    bool load(const TableBlob& blob, std::string_view name);

    [[nodiscard]] std::size_t row_count() const noexcept { return rows_.size(); }
    [[nodiscard]] std::optional<TableRow> row(std::uint64_t key) const noexcept;
    [[nodiscard]] const std::vector<TableRow>& rows() const noexcept { return rows_; }
    [[nodiscard]] std::string name() const { return {name_}; }
    [[nodiscard]] std::span<const std::uint8_t> raw() const noexcept { return raw_; }
    [[nodiscard]] std::optional<std::int32_t> int_field(const TableRow& row, std::uint32_t field) const;
    [[nodiscard]] std::optional<std::string> string_field(const TableRow& row, std::uint32_t field) const;
    [[nodiscard]] std::vector<std::int32_t> ints_field(const TableRow& row, std::uint32_t field) const;

private:
    std::vector<std::uint8_t> owned_;
    std::span<const std::uint8_t> raw_;
    std::vector<TableRow> rows_;
    std::string name_;
};

} // namespace x2::offline
