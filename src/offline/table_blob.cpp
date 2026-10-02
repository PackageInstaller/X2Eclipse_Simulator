#include "offline/table_blob.hpp"

#include <cstring>
#include <utility>

namespace x2::offline {

namespace {
constexpr std::string_view kMagic = "X2DATA1";

bool read_u32(std::span<const std::uint8_t> data, std::size_t& pos, std::uint32_t& out) {
    if (pos + 4 > data.size()) return false;
    out = static_cast<std::uint32_t>(data[pos]) |
          (static_cast<std::uint32_t>(data[pos + 1]) << 8) |
          (static_cast<std::uint32_t>(data[pos + 2]) << 16) |
          (static_cast<std::uint32_t>(data[pos + 3]) << 24);
    pos += 4;
    return true;
}
}

bool TableBlob::load(std::span<const std::uint8_t> blob) {
    entries_.clear();
    if (blob.size() < kMagic.size() + 4 || std::memcmp(blob.data(), kMagic.data(), kMagic.size()) != 0) {
        return false;
    }
    std::size_t pos = kMagic.size();
    std::uint32_t count{};
    if (!read_u32(blob, pos, count)) return false;

    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t name_size{};
        if (!read_u32(blob, pos, name_size) || pos + name_size > blob.size()) return false;
        std::string name(reinterpret_cast<const char*>(blob.data() + pos), name_size);
        pos += name_size;
        std::uint32_t original_size{}, size{};
        if (!read_u32(blob, pos, original_size) || !read_u32(blob, pos, size) || pos + size > blob.size()) {
            return false;
        }
        auto data = blob.subspan(pos, size);
        pos += size;
        entries_.emplace(std::move(name), Entry{std::string(), original_size, size, data});
    }
    return true;
}

void TableBlob::own_blob(std::vector<std::uint8_t> blob) {
    const auto base = blob.data();
    const auto size = blob.size();
    owned_ = std::move(blob);
    load({base, size});
    // Rebase lazily: load() used the caller's temporary span, so rebuild once.
    if (!owned_.empty()) load(owned_);
}

bool TableBlob::own_blob_and_load(std::vector<std::uint8_t> blob) {
    owned_ = std::move(blob);
    return load(owned_);
}

const TableBlob::Entry* TableBlob::find(std::string_view name) const noexcept {
    const auto it = entries_.find(name);
    return it == entries_.end() ? nullptr : &it->second;
}

std::vector<std::string> TableBlob::names() const {
    std::vector<std::string> result;
    result.reserve(entries_.size());
    for (const auto& [name, _] : entries_) result.push_back(name);
    return result;
}

} // namespace x2::offline
