#pragma once

#include <cstdint>

#include "offline/account_state.hpp"
#include "offline/table_blob.hpp"

namespace x2::offline {


[[nodiscard]] bool is_medal_item(const TableBlob* tables, std::int32_t item_id);
void grant_medal(Account& account, std::int32_t medal_id, std::int64_t now_unix);
bool migrate_bag_medals(Account& account, const TableBlob* tables, std::int64_t now_unix);
[[nodiscard]] std::int32_t blood_moon_medal(const TableBlob* tables, std::int32_t section);

} // namespace x2::offline
