#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "offline/account_state.hpp"
#include "offline/table_blob.hpp"

namespace x2::offline {

[[nodiscard]] std::vector<std::uint8_t> encode_player(const AccountPlayer& player);
[[nodiscard]] AccountPlayer decode_player(std::span<const std::uint8_t> blob);

class AccountRepository {
public:
    void set_tables(std::shared_ptr<TableBlob> tables);
    [[nodiscard]] Account create_or_get(std::string_view account, std::string_view token);
    [[nodiscard]] std::optional<Account> find(std::string_view account) const;
    void save(std::string_view account, const Account& state);

private:
    [[nodiscard]] Account build_fresh(std::string_view account) const;
    void hydrate();

    std::shared_ptr<TableBlob> tables_;
    mutable std::mutex mutex_;
    std::map<std::string, Account, std::less<>> accounts_;
};

} // namespace x2::offline
