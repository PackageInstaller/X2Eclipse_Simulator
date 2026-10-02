#pragma once

#include <cstdint>
#include <vector>

#include "offline/account_state.hpp"
#include "offline/table_blob.hpp"

namespace x2::offline {


[[nodiscard]] std::vector<std::uint8_t> query_npc_blog(const Account& account, const TableBlob* tables,
                                                       std::int64_t now_unix);


[[nodiscard]] std::vector<std::uint8_t> like_npc_blog(Account& account, const TableBlob* tables,
                                                      std::int32_t hero_id, std::int32_t group_id,
                                                      std::int64_t now_unix);


[[nodiscard]] std::vector<std::uint8_t> reply_npc_blog(Account& account, const TableBlob* tables,
                                                       std::int32_t hero_id, std::int32_t group_id,
                                                       std::int32_t chat_group_id, std::int32_t reply_id,
                                                       std::int64_t now_unix);


[[nodiscard]] std::vector<std::uint8_t> query_npc_letter(const Account& account, const TableBlob* tables,
                                                         std::int64_t now_unix);
[[nodiscard]] std::vector<std::uint8_t> update_private_letter(const Account& account, const TableBlob* tables,
                                                              std::int32_t hero_id, std::int64_t now_unix);

} // namespace x2::offline
