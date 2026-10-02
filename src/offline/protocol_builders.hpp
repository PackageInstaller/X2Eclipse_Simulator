#pragma once

#include <cstdint>
#include <vector>

#include "offline/account_state.hpp"
#include "offline/table_blob.hpp"

namespace x2::offline {


[[nodiscard]] constexpr std::int32_t client_time(std::int64_t unix_seconds) {
    return static_cast<std::int32_t>(unix_seconds - 1514736000);
}

class ProtocolBuilders {
public:
    [[nodiscard]] static std::vector<std::uint8_t> item_data(const AccountItem& item);
    [[nodiscard]] static std::vector<std::uint8_t> hero_equip(const AccountEquip& equip);
    [[nodiscard]] static std::vector<std::uint8_t> hero_data(const AccountHero& hero,
                                                            const TableBlob* tables = nullptr);
    [[nodiscard]] static std::vector<std::uint8_t> hero_all(const Account& account,
                                                           const TableBlob* tables = nullptr);
    [[nodiscard]] static std::vector<std::uint8_t> item_all(const Account& account);
    [[nodiscard]] static std::vector<std::uint8_t> equip_all(const Account& account);
    [[nodiscard]] static std::vector<std::uint8_t> login(Account& account, std::int32_t server_time,
                                                         const TableBlob* tables = nullptr);
    [[nodiscard]] static std::vector<std::uint8_t> fight_profile(const TableBlob* tables, const Account& account);
    [[nodiscard]] static std::vector<std::uint8_t> reconnect(const Account& account, std::int32_t server_time);
    [[nodiscard]] static std::vector<std::uint8_t> player_data(const Account& account, std::int32_t server_time,
                                                              const TableBlob* tables = nullptr);
    [[nodiscard]] static std::vector<std::uint8_t> query_activity(const TableBlob& tables, std::int32_t server_time);
    [[nodiscard]] static std::vector<std::int32_t> icon_unlock_ids(const AccountHero& hero,
                                                                   const TableBlob* tables,
                                                                   const Account* account = nullptr);
    [[nodiscard]] static std::vector<std::uint8_t> growth_base(const Account& account,
                                                               const TableBlob* tables);
    [[nodiscard]] static std::vector<std::uint8_t> fight_data(const TableBlob* tables, const Account& account,
                                                              const std::vector<std::int32_t>& hero_ids,
                                                              std::int32_t mission);
    [[nodiscard]] static std::vector<std::uint8_t> query_mission(const TableBlob* tables, const Account& account);
    [[nodiscard]] static std::vector<std::uint8_t> card_pool(const TableBlob& tables, const Account& account,
                                                             std::int32_t server_time);
    [[nodiscard]] static std::vector<std::uint8_t> inside_battle_shop(const TableBlob& tables, std::int32_t shop_id,
                                                                      std::int32_t floor, std::int32_t section);
    [[nodiscard]] static std::int32_t inside_battle_price(const TableBlob& tables, std::int32_t item_id);
};

} // namespace x2::offline
