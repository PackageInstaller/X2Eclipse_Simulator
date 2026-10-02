#include "offline/medal.hpp"

#include "offline/game_table.hpp"

namespace x2::offline {

bool is_medal_item(const TableBlob* tables, std::int32_t item_id) {
    if (!tables || item_id <= 0) return false;
    GameTable item;
    if (item.load(*tables, "Item")) {
        if (const auto row = item.row(static_cast<std::uint64_t>(item_id))) {
            return item.int_field(*row, 6).value_or(0) == 20; // ItemEItemType.E_Medal
        }
    }
    GameTable medal;
    return medal.load(*tables, "Medal") && medal.row(static_cast<std::uint64_t>(item_id)).has_value();
}

void grant_medal(Account& account, std::int32_t medal_id, std::int64_t now_unix) {
    if (medal_id <= 0 || now_unix <= 0) return;
    account.player.medal_pack.emplace(medal_id, now_unix);
}

bool migrate_bag_medals(Account& account, const TableBlob* tables, const std::int64_t now_unix) {
    bool changed = false;
    std::erase_if(account.items, [&](const AccountItem& entry) {
        if (!is_medal_item(tables, entry.id)) return false;
        grant_medal(account, entry.id, now_unix);
        changed = true;
        return true;
    });
    return changed;
}

std::int32_t blood_moon_medal(const TableBlob* tables, std::int32_t section) {
    (void)tables;
    return section == 2110860 ? 1260006 : 0;
}

} // namespace x2::offline
