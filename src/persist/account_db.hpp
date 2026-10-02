#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace x2::persist {

struct AccountRow {
    std::string token;
    std::int64_t id{};
    std::int32_t login_count{};
    std::int32_t is_create_role{};
    std::vector<std::uint8_t> heroes;
    std::vector<std::uint8_t> items;
    std::vector<std::uint8_t> player;
};

bool init_database();

void save_account(const AccountRow& row);
std::vector<AccountRow> load_accounts();

} // namespace x2::persist
