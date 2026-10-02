#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace x2::offline {

class TableBlob {
public:
    struct Entry {
        std::string name;
        std::uint32_t original_size{};
        std::uint32_t size{};
        std::span<const std::uint8_t> data;
    };

    bool load(std::span<const std::uint8_t> blob);
    void own_blob(std::vector<std::uint8_t> blob);
    bool own_blob_and_load(std::vector<std::uint8_t> blob);

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] const Entry* find(std::string_view name) const noexcept;
    [[nodiscard]] std::vector<std::string> names() const;

private:
    std::vector<std::uint8_t> owned_;
    std::map<std::string, Entry, std::less<>> entries_;
};

} // namespace x2::offline
