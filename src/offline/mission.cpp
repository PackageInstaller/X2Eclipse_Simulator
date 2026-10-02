#include "offline/mission.hpp"

#include <algorithm>
#include <ctime>
#include <limits>
#include <optional>
#include <random>

#include "offline/game_table.hpp"
#include "proto/protobuf.hpp"

namespace x2::offline {

std::int32_t* wallet(AccountPlayer& player, std::int32_t enu) {
    switch (enu) {
        case E_Power: return &player.power;
        case E_Gold: return &player.gold;
        case E_Money: return &player.crystal;
        case E_JewelChip: return &player.jewel_chip;
        case E_EquibExp: return &player.equip_exp;
        case E_HeroExp: return &player.hero_exp;
        case E_RoleExp: return &player.exp;
        case E_DailyActiveValue: return &player.daily_activity;
        case E_PowerOfLight: return &player.power_of_light;
        case E_VowOfCoin: return &player.vow_coin;
        case E_Crystal: return &player.wish_crystal;
        case E_SkillPoint: return &player.skill_point;
        case E_FriendCoin: return &player.friend_coin;
        case E_SkinTicket: return &player.skin_coupon;
        default: return nullptr;
    }
}

std::optional<std::int32_t> item_currency_enu(const TableBlob* tables, std::int32_t item_id) {
    if (tables) {
        GameTable item, currency;
        if (item.load(*tables, "Item") && currency.load(*tables, "CurrencyType")) {
            if (const auto row = item.row(static_cast<std::uint64_t>(item_id));
                row && item.int_field(*row, 21).value_or(0) == 2) {
                const auto types = item.ints_field(*row, 22);
                if (const auto type = types.empty() ? std::nullopt : currency.row(static_cast<std::uint64_t>(types[0]))) {
                    return currency.int_field(*type, 4).value_or(0);
                }
            }
        }
    }
    if (item_id == 1237900) return E_Power;
    if (item_id == 1237901) return E_Gold;
    if (item_id == 1237902) return E_Money;
    if (item_id == 1237904) return E_JewelChip;
    if (item_id == 1237906) return E_EquibExp;
    if (item_id == 1237907) return E_HeroExp;
    if (item_id == 1237908) return E_RoleExp;
    if (item_id == 1237910) return E_DailyActiveValue;
    if (item_id == 1237913) return E_PowerOfLight;
    if (item_id == 1237914) return E_VowOfCoin;
    if (item_id == 1237915) return E_Crystal;
    if (item_id == 1237916) return E_SkillPoint;
    if (item_id == 1237923) return E_SkinTicket;
    return std::nullopt;
}

std::int32_t clamp_add(std::int32_t base, std::int64_t delta) {
    return static_cast<std::int32_t>(
        std::clamp<std::int64_t>(base + delta, 0, std::numeric_limits<std::int32_t>::max()));
}

namespace {

using proto::Reader;
using proto::Writer;

// SectionTable 字段
constexpr std::uint32_t kSectionManualValue = 44; // power cost
constexpr std::uint32_t kSectionFirstReward = 45; // firVReward: Gift groups
constexpr std::uint32_t kSectionExpertChestReward = 46; // expertChestReward: Item id (chest)
constexpr std::uint32_t kSectionChestReward = 47;       // chestReward: Item id (chest)
constexpr std::uint32_t kSectionReward = 48;      // vReward: Gift groups
constexpr std::uint32_t kSectionDroopLimit = 25;  // items the battle may drop
constexpr std::uint32_t kSectionDroopLimit3 = 27; // equip star bounds [min_star, max_star]
constexpr std::uint32_t kSectionMopReward = 49;   // sweep: Gift groups
constexpr std::int32_t kReportCurrency = 14;      // Item.functionEff E_ReportCurrency: effData [CurrencyType, amount]
constexpr std::int32_t kTaskKillMonster = 1, kTaskCustomsPass = 3, kTaskSignIn = 5, kTaskOnlineAt = 6,
                       kTaskHeroLevelUp = 7, kTaskConsumePower = 10;

std::span<const std::uint8_t> bytes_or_empty(Reader& reader) {
    return reader.bytes().value_or(std::span<const std::uint8_t>{});
}


std::mt19937& rng() {
    static std::mt19937 engine{std::random_device{}()};
    return engine;
}


std::int32_t equip_star(const GameTable& item, const AccountItem& reward) {
    if (reward.show >= 1 && reward.show <= 6) return reward.show;
    const auto show = item.row(static_cast<std::uint64_t>(reward.show));
    if (!show || item.int_field(*show, 6).value_or(0) != 13) return 1;
    return std::max(1, item.int_field(*show, 7).value_or(1));
}

std::int32_t settle_role_level(Account& account, const TableBlob* tables) {
    if (!tables) return 0;
    GameTable role;
    if (!role.load(*tables, "RoleExp")) return 0;
    auto& p = account.player;
    const auto before = p.level;
    while (const auto row = role.row(static_cast<std::uint64_t>(p.level))) {
        const auto need = role.int_field(*row, 2).value_or(0);
        if (need <= 0 || p.exp < need || !role.row(static_cast<std::uint64_t>(p.level + 1))) break;
        p.exp -= need;
        ++p.level;
    }
    if (p.level <= before) return 0;
    grant_role_level_gifts(account, tables, std::max(before, p.rewarded_level), p.level);
    p.rewarded_level = std::max(p.rewarded_level, p.level);
    seed_guides(account, *tables);
    return p.level - before;
}


} // namespace
void expand_gift(const GameTable& gift, std::int32_t group, std::vector<AccountItem>& out) {
    const auto row = gift.row(static_cast<std::uint64_t>(group));
    if (!row) return;
    const auto items = gift.ints_field(*row, 4);
    const auto nums = gift.ints_field(*row, 5);
    const auto shows = gift.ints_field(*row, 6);
    const auto grant = [&](std::size_t i) {
        if (i < items.size())
            out.push_back({.id = items[i], .num = i < nums.size() ? nums[i] : 1,
                           .show = shows.empty() ? 0 : shows[std::min(i, shows.size() - 1)]});
    };
    if (gift.int_field(*row, 2).value_or(1) == 1) {
        for (std::size_t i = 0; i < items.size(); ++i) grant(i);
        return;
    }
    auto weights = gift.ints_field(*row, 3);
    weights.resize(items.size(), 1);
    if (std::ranges::all_of(weights, [](std::int32_t w) { return w <= 0; })) return;
    std::discrete_distribution<std::size_t> pick(weights.begin(), weights.end());
    for (std::int32_t n = std::max(1, gift.int_field(*row, 7).value_or(1)); n > 0; --n) grant(pick(rng()));
}


Paid pay_rewards(Account& account, const TableBlob* tables, const std::vector<AccountItem>& rewards) {
    Paid paid;
    GameTable item, currency;
    const bool has_tables = tables && item.load(*tables, "Item") && currency.load(*tables, "CurrencyType");
    auto& p = account.player;
    for (const auto& reward : rewards) {
        if (reward.id <= 0 || reward.num <= 0) continue;
        const auto irow = has_tables ? item.row(static_cast<std::uint64_t>(reward.id)) : std::nullopt;
        if (irow && item.int_field(*irow, 6).value_or(0) == 10) {
            for (std::int32_t n = 0; n < reward.num; ++n) {
                std::int32_t id = 1;
                for (const auto& owned : p.equips) id = std::max(id, owned.id + 1);
                p.equips.push_back(AccountEquip{.id = id, .type_id = reward.id, .star = equip_star(item, reward)});
                paid.equips.push_back(p.equips.back());
            }
            continue;
        }
        paid.shown.push_back(reward);
        // Item.itemType E_Medal=20: 徽章进 MedalSystem (PlayerData.f7)
        if (is_medal_item(tables, reward.id)) {
            grant_medal(account, reward.id, static_cast<std::int64_t>(std::time(nullptr)));
            paid.player = true;
            continue;
        }
        std::optional<std::int32_t> enu;
        if (irow && item.int_field(*irow, 21).value_or(0) == 2) {
            const auto types = item.ints_field(*irow, 22);
            if (const auto type = types.empty() ? std::nullopt : currency.row(static_cast<std::uint64_t>(types[0]))) {
                enu = currency.int_field(*type, 4).value_or(0);
            }
        }
        if (!enu) {
            enu = item_currency_enu(tables, reward.id);
        }
        if (enu) {
            paid.player = true;
            switch (*enu) {
                case E_Power: p.power = clamp_add(p.power, reward.num); break;
                case E_Gold: p.gold = clamp_add(p.gold, reward.num); break;
                case E_Money: p.crystal = clamp_add(p.crystal, reward.num); break;
                case E_JewelChip: p.jewel_chip = clamp_add(p.jewel_chip, reward.num); break;
                case E_HeroExp: p.hero_exp = clamp_add(p.hero_exp, reward.num); break;
                case E_RoleExp: p.exp = clamp_add(p.exp, reward.num); paid.role_exp = true; break;
                case E_DailyActiveValue: p.daily_activity = clamp_add(p.daily_activity, reward.num); break;
                case E_SkillPoint: p.skill_point = clamp_add(p.skill_point, reward.num); break;
                default: if (auto* field = wallet(p, *enu)) *field = clamp_add(*field, reward.num); break;
            }
            continue;
        }
        auto it = std::ranges::find(account.items, reward.id, &AccountItem::id);
        if (it == account.items.end()) it = account.items.insert(account.items.end(), AccountItem{.id = reward.id});
        it->num = clamp_add(it->num, reward.num);
        paid.bag.push_back(*it);
    }
    return paid;
}


std::int32_t max_role_level(const TableBlob* tables) {
    if (!tables) return 1;
    GameTable role;
    if (!role.load(*tables, "RoleExp")) return 1;
    std::int32_t max_level = 1;
    for (const auto& row : role.rows()) max_level = std::max(max_level, role.int_field(row, 1).value_or(1));
    return max_level;
}

void grant_role_level_gifts(Account& account, const TableBlob* tables, std::int32_t from_level, std::int32_t to_level) {
    if (!tables || to_level < from_level) return;
    GameTable role, gift, item, currency;
    if (!role.load(*tables, "RoleExp")) return;
    gift.load(*tables, "Gift");
    item.load(*tables, "Item");
    currency.load(*tables, "CurrencyType");
    auto& p = account.player;
    const auto power_row = currency.row(900);
    const auto power_max = power_row ? currency.int_field(*power_row, 6).value_or(9999) : 9999;
    const auto power_before = p.power;
    for (std::int32_t level = from_level + 1; level <= to_level; ++level) {
        const auto row = role.row(static_cast<std::uint64_t>(level));
        if (!row) continue;
        p.power = clamp_add(p.power, role.int_field(*row, 4).value_or(0));
        std::vector<AccountItem> rewards;
        expand_gift(gift, role.int_field(*row, 5).value_or(0), rewards);
        for (const auto& reward : rewards) {
            std::optional<std::int32_t> enu;
            if (const auto irow = item.row(static_cast<std::uint64_t>(reward.id));
                irow && item.int_field(*irow, 21).value_or(0) == 2) {
                const auto types = item.ints_field(*irow, 22);
                if (const auto type = types.empty() ? std::nullopt : currency.row(static_cast<std::uint64_t>(types[0]))) {
                    enu = currency.int_field(*type, 4).value_or(0);
                }
            }
            if (enu && *enu == E_Power) p.power = clamp_add(p.power, reward.num);
        }
    }
    p.power = std::max(power_before, std::min(p.power, power_max));
}

CollectionClaim claim_collection_award(Account& account, const TableBlob* tables, std::int32_t award_id) {
    if (!tables || award_id <= 0) return {};
    if (std::ranges::contains(account.player.collection_awards, award_id)) return {.ok = true, .already = true};
    GameTable collection, gift, item, currency;
    if (!collection.load(*tables, "Collection")) return {};
    const auto row = collection.row(static_cast<std::uint64_t>(award_id));
    if (!row) return {};
    gift.load(*tables, "Gift");
    item.load(*tables, "Item");
    currency.load(*tables, "CurrencyType");
    std::vector<AccountItem> rewards;
    expand_gift(gift, collection.int_field(*row, 6).value_or(0), rewards);
    CollectionClaim claim{.ok = true};
    auto& p = account.player;
    for (const auto& reward : rewards) {
        if (reward.id <= 0 || reward.num <= 0) continue;
        claim.shown.push_back(reward);
        std::optional<std::int32_t> enu;
        if (const auto irow = item.row(static_cast<std::uint64_t>(reward.id));
            irow && item.int_field(*irow, 21).value_or(0) == 2) {
            const auto types = item.ints_field(*irow, 22);
            if (const auto type = types.empty() ? std::nullopt : currency.row(static_cast<std::uint64_t>(types[0]))) {
                enu = currency.int_field(*type, 4).value_or(0);
            }
        }
        if (enu) {
            claim.player = true;
            switch (*enu) {
                case E_Power: p.power = clamp_add(p.power, reward.num); break;
                case E_Gold: p.gold = clamp_add(p.gold, reward.num); break;
                case E_Money: p.crystal = clamp_add(p.crystal, reward.num); break;
                case E_JewelChip: p.jewel_chip = clamp_add(p.jewel_chip, reward.num); break;
                case E_HeroExp: p.hero_exp = clamp_add(p.hero_exp, reward.num); break;
                case E_SkillPoint: p.skill_point = clamp_add(p.skill_point, reward.num); break;
                default: if (auto* field = wallet(p, *enu)) *field = clamp_add(*field, reward.num); break;
            }
            continue;
        }
        auto it = std::ranges::find(account.items, reward.id, &AccountItem::id);
        if (it == account.items.end()) it = account.items.insert(account.items.end(), AccountItem{.id = reward.id});
        it->num = clamp_add(it->num, reward.num);
        claim.bag.push_back(*it);
    }
    p.collection_awards.push_back(award_id);
    return claim;
}

CheckoutRequest parse_checkout(std::span<const std::uint8_t> body) {
    CheckoutRequest request;
    Reader reader{body};
    while (!reader.eof()) {
        const auto field = reader.field();
        if (!field) break;
        if (field->wire_type == 0 && (field->number == 1 || field->number == 2 || field->number == 4 || field->number == 6)) {
            const auto value = static_cast<std::int32_t>(reader.varint().value_or(0));
            if (field->number == 1) request.chapter = value;
            else if (field->number == 2) request.section = value;
            else if (field->number == 4) request.success = value != 0;
            else if (field->number == 6) request.expert_mode = value != 0;
        } else if (field->wire_type == 2 && (field->number == 3 || field->number == 10 || field->number == 12)) {
            Reader nested{bytes_or_empty(reader)};
            AccountItem item;
            while (!nested.eof()) {
                const auto f = nested.field();
                if (!f) break;
                if (f->wire_type != 0) {
                    if (!nested.skip(f->wire_type)) break;
                    continue;
                }
                const auto value = static_cast<std::int32_t>(nested.varint().value_or(0));
                if (f->number == 1) item.id = value;
                else if (f->number == 2) item.num = value;
            }
            if (field->number == 10) request.heroes.push_back(item.id);
            else if (item.id > 0 && item.num > 0) request.maze_items.push_back(item);
        } else if (field->wire_type == 2 && field->number == 22) {
            // FightKillMonster: 1 normal, 2 boss, 3 elite
            Reader nested{bytes_or_empty(reader)};
            while (!nested.eof()) {
                const auto f = nested.field();
                if (!f) break;
                if (f->wire_type != 0) {
                    if (!nested.skip(f->wire_type)) break;
                    continue;
                }
                request.kills = clamp_add(request.kills, static_cast<std::int64_t>(nested.varint().value_or(0)));
            }
        } else if (!reader.skip(field->wire_type)) {
            break;
        }
    }
    return request;
}

std::int32_t section_power_cost(const TableBlob& tables, std::int32_t section) {
    GameTable table;
    if (!table.load(tables, "SectionTable")) return 0;
    const auto row = table.row(static_cast<std::uint64_t>(section));
    return row ? table.int_field(*row, kSectionManualValue).value_or(0) : 0;
}

std::vector<std::int32_t> drop_values(const TableBlob& tables, std::int32_t section_id) {
    GameTable section, gift, item, currency;
    section.load(tables, "SectionTable");
    gift.load(tables, "Gift");
    item.load(tables, "Item");
    currency.load(tables, "CurrencyType");
    std::vector<std::int32_t> values;
    const auto row = section.row(static_cast<std::uint64_t>(section_id));
    if (!row) return values;
    std::vector<AccountItem> sweep;
    for (const auto group : section.ints_field(*row, kSectionMopReward)) expand_gift(gift, group, sweep);
    for (const auto id : section.ints_field(*row, kSectionDroopLimit)) {
        const auto irow = item.row(static_cast<std::uint64_t>(id));
        if (!irow || item.int_field(*irow, 21).value_or(0) != kReportCurrency) continue;
        const auto eff = item.ints_field(*irow, 22);
        const auto type = eff.size() == 2 && eff[1] > 0 ? currency.row(static_cast<std::uint64_t>(eff[0])) : std::nullopt;
        if (!type) continue;
        const auto coin = currency.int_field(*type, 5).value_or(0);
        std::int64_t total = 0;
        for (const auto& reward : sweep) total += reward.id == coin ? reward.num : 0;
        const auto group = std::max(0, item.int_field(*irow, 10).value_or(0)); // AddADCGroup
        if (std::ssize(values) <= group) values.resize(static_cast<std::size_t>(group) + 1);
        const auto budget = clamp_add(0, total * item.int_field(*irow, 9).value_or(0) / eff[1]); // ItemValue
        values[static_cast<std::size_t>(group)] = std::max(values[static_cast<std::size_t>(group)], budget);
    }
    return values;
}

CheckoutResult apply_checkout(Account& account, const TableBlob& tables, const CheckoutRequest& request) {
    CheckoutResult result;
    auto& p = account.player;
    if (!request.success) {
        result.returned_power = section_power_cost(tables, request.section);
        p.power = clamp_add(p.power, result.returned_power);
        p.pending_section = 0;
        p.pending_chapter = 0;
        return result;
    }
    count_daily_task(account, tables, kTaskCustomsPass, request.section);
    count_daily_task(account, tables, kTaskConsumePower, 900, section_power_cost(tables, request.section));
    count_daily_task(account, tables, kTaskKillMonster, 0, request.kills);

    GameTable section, gift, item, currency;
    section.load(tables, "SectionTable");
    gift.load(tables, "Gift");
    item.load(tables, "Item");
    currency.load(tables, "CurrencyType");
    result.first_clear = !std::ranges::contains(p.cleared_main, request.section);
    if (const auto row = section.row(static_cast<std::uint64_t>(request.section))) {
        if (result.first_clear) {
            for (const auto group : section.ints_field(*row, kSectionFirstReward)) expand_gift(gift, group, result.rewards);
        }
        for (const auto group : section.ints_field(*row, kSectionReward)) expand_gift(gift, group, result.rewards);

        std::int32_t chest_id = 0;
        if (request.expert_mode) {
            if (const auto cid = section.int_field(*row, kSectionExpertChestReward); cid && *cid > 0) {
                chest_id = *cid;
            }
        }
        if (chest_id <= 0) {
            if (const auto cid = section.int_field(*row, kSectionChestReward); cid && *cid > 0) {
                chest_id = *cid;
            }
        }
        if (chest_id > 0) {
            if (const auto crow = item.row(static_cast<std::uint64_t>(chest_id))) {
                for (const auto gift_id : item.ints_field(*crow, 16)) {
                    expand_gift(gift, gift_id, result.rewards);
                }
            }
        }

        const auto droop_limits = section.ints_field(*row, kSectionDroopLimit);
        const auto droop_limit3 = section.ints_field(*row, kSectionDroopLimit3);
        if (!droop_limits.empty() && droop_limit3.size() >= 2) {
            std::vector<std::int32_t> equip_pool;
            for (const auto id : droop_limits) {
                if (const auto irow = item.row(static_cast<std::uint64_t>(id));
                    irow && item.int_field(*irow, 6).value_or(0) == 10) {
                    equip_pool.push_back(id);
                }
            }
            if (!equip_pool.empty()) {
                const auto min_star = std::clamp(droop_limit3[0], 1, 6);
                const auto max_star = std::clamp(droop_limit3[1], min_star, 6);
                const auto count = std::uniform_int_distribution<std::int32_t>{0, 1}(rng()) == 0 ? 1 : 2;
                std::uniform_int_distribution<std::size_t> equip_dist{0, equip_pool.size() - 1};
                for (std::int32_t i = 0; i < count; ++i) {
                    const auto equip_id = equip_pool[equip_dist(rng())];
                    std::int32_t star = max_star;
                    if (min_star < max_star) {
                        std::vector<double> star_weights;
                        for (std::int32_t s = min_star; s <= max_star; ++s) {
                            star_weights.push_back(static_cast<double>(s - min_star + 1));
                        }
                        const auto s_idx = std::discrete_distribution<std::size_t>{
                            star_weights.begin(), star_weights.end()}(rng());
                        star = min_star + static_cast<std::int32_t>(s_idx);
                    }
                    result.rewards.push_back(AccountItem{.id = equip_id, .num = 1, .show = star});
                }
            }
        }
    }
    for (auto picked : request.maze_items) {
        const auto row = item.row(static_cast<std::uint64_t>(picked.id));
        if (!row) continue;
        if (item.int_field(*row, 6).value_or(0) == 4) {
            if (!std::ranges::contains(p.relic_pack, picked.id)) p.relic_pack.push_back(picked.id);
            continue;
        }
        if (item.int_field(*row, 5).value_or(0) == 0) continue;
        const auto eff = item.ints_field(*row, 22);
        if (item.int_field(*row, 21).value_or(0) == kReportCurrency && eff.size() == 2) {
            const auto type = currency.row(static_cast<std::uint64_t>(eff[0]));
            if (!type) continue;
            picked = {.id = currency.int_field(*type, 5).value_or(0), .num = clamp_add(0, std::int64_t{eff[1]} * picked.num)};
        }
        const auto same = std::ranges::find(result.rewards, picked.id, &AccountItem::id);
        if (same == result.rewards.end()) result.rewards.push_back(picked);
        else same->num = clamp_add(same->num, picked.num);
    }


    const auto grant = [&](const AccountItem& reward) {
        std::optional<std::int32_t> enu;
        if (const auto row = item.row(static_cast<std::uint64_t>(reward.id));
            row && item.int_field(*row, 21).value_or(0) == 2) {
            const auto types = item.ints_field(*row, 22);
            if (const auto type = types.empty() ? std::nullopt : currency.row(static_cast<std::uint64_t>(types[0]))) {
                enu = currency.int_field(*type, 4).value_or(0);
            }
        }
        if (enu) {
            switch (*enu) {
                case E_Power: p.power = clamp_add(p.power, reward.num); break;
                case E_Gold: p.gold = clamp_add(p.gold, reward.num); break;
                case E_Money: p.crystal = clamp_add(p.crystal, reward.num); break;
                case E_Silver: break; // in-battle currency, not kept after the battle
                case E_JewelChip: p.jewel_chip = clamp_add(p.jewel_chip, reward.num); break;
                case E_EquibExp: p.equip_exp = clamp_add(p.equip_exp, reward.num); break;
                case E_HeroExp: p.hero_exp = clamp_add(p.hero_exp, reward.num); break;
                case E_RoleExp: p.exp = clamp_add(p.exp, reward.num); break;
                case E_SkillPoint: p.skill_point = clamp_add(p.skill_point, reward.num); break;
                default: // no wallet field: still listed in rewards, not a bag item
                    if (auto* field = wallet(p, *enu)) *field = clamp_add(*field, reward.num);
                    break;
            }
            return;
        }

        const auto row = item.row(static_cast<std::uint64_t>(reward.id));
        const auto type = row ? item.int_field(*row, 6).value_or(0) : 0;
        const auto eff = row ? item.int_field(*row, 21).value_or(0) : 0;
                if (type == 10) {
                    std::int32_t id = 1;
                    for (const auto& owned : p.equips) id = std::max(id, owned.id + 1);
                    AccountEquip equip{.id = id, .type_id = reward.id, .star = equip_star(item, reward)};
            p.equips.push_back(equip);
            result.reward_equips.push_back(equip);
            return;
        }
        if (eff == 4) { // ItemEFunctionEff.E_Hero: grant the unit, not a bag stack.
            const auto eds = row ? item.ints_field(*row, 22) : std::vector<std::int32_t>{};
            const auto hid = eds.empty() ? reward.id : eds[0];
            auto owned = std::ranges::find(account.heroes, hid, &AccountHero::id);
            if (owned == account.heroes.end()) {
                account.heroes.push_back(AccountHero{
                    .id = hid, .state = 2, .level = 1, .star = default_hero_star(&tables, hid)});
                owned = account.heroes.end() - 1;
            }
            owned->state = 2;
            result.changed_heroes.push_back(*owned);
            return;
        }

        if (type == 16) {
            if (const auto enu = item_currency_enu(&tables, reward.id)) {
                if (auto* field = wallet(account.player, *enu)) {
                    *field = clamp_add(*field, reward.num);
                }
            }
            return;
        }
        if (false) {
            grant_medal(account, reward.id, static_cast<std::int64_t>(std::time(nullptr)));
            return;
        }
        auto it = std::ranges::find(account.items, reward.id, &AccountItem::id);
        if (it == account.items.end()) it = account.items.insert(account.items.end(), AccountItem{.id = reward.id});
        it->num = clamp_add(it->num, reward.num);
        result.changed_items.push_back(*it);
    };
    for (const auto& reward : result.rewards) grant(reward);
    std::erase_if(result.rewards, [&](const AccountItem& reward) {
        const auto row = item.row(static_cast<std::uint64_t>(reward.id));
        return row && item.int_field(*row, 6).value_or(0) == 10;
    });

    if (result.first_clear) p.cleared_main.push_back(request.section);
    if (request.success) {
        if (const auto medal_id = blood_moon_medal(&tables, request.section); medal_id > 0) {
            grant_medal(account, medal_id, static_cast<std::int64_t>(std::time(nullptr)));
        }
    }

    if (const auto row = section.row(static_cast<std::uint64_t>(request.section));
        row && section.int_field(*row, 8).value_or(0) == 0) {
        p.main_chapter = request.chapter;
        p.main_section = request.section;
    }
    p.pending_section = 0;
    p.pending_chapter = 0;
    result.level_ups = settle_role_level(account, &tables);
    return result;
}

bool drop_maze_items(Account& account, const TableBlob& tables) {
    GameTable item;
    if (!item.load(tables, "Item")) return false;
    return std::erase_if(account.items, [&](const AccountItem& entry) {
        const auto row = item.row(static_cast<std::uint64_t>(entry.id));
        if (!row) return false;
        const auto scene = item.int_field(*row, 5).value_or(0);
        const auto type = item.int_field(*row, 6).value_or(0);
        if (type == 10) {
            if (!std::ranges::any_of(account.player.equips, [&](const AccountEquip& e) { return e.type_id == entry.id; })) {
                account.player.equips.push_back({.id = entry.id, .type_id = entry.id});
            }
            return true;
        }
        return scene == 0; // E_Maze bag leak
    }) > 0;
}

void seed_guides(Account& account, const TableBlob& tables) {
    GameTable table; // TitoGuideTable: 2 groupID, 3 nextGroupID
    if (!table.load(tables, "TitoGuideTable")) return;
    std::map<std::int32_t, std::int32_t> open_level;
    if (GameTable open; open.load(tables, "FunctionOpen")) {
        for (const auto& row : open.rows()) {
            const auto group = open.int_field(row, 7).value_or(0);
            if (group <= 0) continue;
            const auto level = open.int_field(row, 4).value_or(0);
            const auto [it, inserted] = open_level.try_emplace(group, level);
            if (!inserted) it->second = std::min(it->second, level);
        }
    }
    std::vector<std::int32_t> groups, nexts;
    for (const auto& row : table.rows()) {
        groups.push_back(table.int_field(row, 2).value_or(0));
        nexts.push_back(table.int_field(row, 3).value_or(0));
    }
    for (const auto group : groups) {
        if (group <= 0 || std::ranges::contains(nexts, group)) continue;
        if (const auto it = open_level.find(group); it != open_level.end() && it->second > account.player.level) continue;
        account.player.guides.try_emplace(group, 0);
    }
}

std::vector<std::int32_t> unlocked_stars(const Account& account, const TableBlob& tables) {
    GameTable stars, chapters, lang;
    if (!stars.load(tables, "StarChartsBase") || !chapters.load(tables, "ChapterInfo")) return {};
    lang.load(tables, "Language");
    struct Chapter {
        std::string name;
        std::vector<std::int32_t> stages;
    };
    std::vector<Chapter> named;
    for (const auto& row : chapters.rows()) {
        const auto text = lang.row(static_cast<std::uint64_t>(chapters.int_field(row, 4).value_or(0)));
        auto name = text ? lang.string_field(*text, 2).value_or("") : std::string{};
        auto stages = chapters.ints_field(row, 2);
        if (!name.empty() && !stages.empty()) named.push_back(Chapter{std::move(name), std::move(stages)});
    }
    std::vector<std::int32_t> open;
    for (const auto& row : stars.rows()) {
        if (stars.int_field(row, 14).value_or(0) != 1) continue; // StarChartsBaseEOpenType.E_Yes
        const auto id = stars.int_field(row, 1).value_or(0);
        const auto text = lang.row(static_cast<std::uint64_t>(stars.int_field(row, 6).value_or(0)));
        const auto desc = text ? lang.string_field(*text, 2).value_or("") : std::string{};
        const Chapter* match = nullptr;
        for (const auto& chapter : named) {
            if (!desc.contains(chapter.name)) continue;
            if (match == nullptr || chapter.name.size() > match->name.size()) match = &chapter;
        }
        if (match == nullptr) continue;
        if (std::ranges::all_of(match->stages, [&](std::int32_t stage) {
                return std::ranges::contains(account.player.cleared_main, stage);
            })) {
            open.push_back(id);
        }
    }
    return open;
}

namespace {


std::vector<std::vector<std::int32_t>> nested_ints(std::span<const std::uint8_t> bytes, std::uint32_t field) {
    std::vector<std::vector<std::int32_t>> lists;
    Reader reader{bytes};
    while (!reader.eof()) {
        const auto outer = reader.field();
        if (!outer) break;
        if (outer->number != field || outer->wire_type != 2) {
            if (!reader.skip(outer->wire_type)) break;
            continue;
        }
        std::vector<std::int32_t> nums;
        Reader inner{reader.bytes().value_or(std::span<const std::uint8_t>{})};
        while (!inner.eof()) {
            const auto item = inner.field();
            if (!item) break;
            if (item->number == 1 && item->wire_type == 0) {
                nums.push_back(static_cast<std::int32_t>(inner.varint().value_or(0)));
            } else if (!inner.skip(item->wire_type)) {
                break;
            }
        }
        lists.push_back(std::move(nums));
    }
    return lists;
}

std::optional<std::int32_t> currency_of(const GameTable& item, const GameTable& currency, std::int32_t id) {
    const auto row = item.row(static_cast<std::uint64_t>(id));
    if (!row || item.int_field(*row, 21).value_or(0) != 2) return std::nullopt;
    const auto types = item.ints_field(*row, 22);
    if (types.empty()) return std::nullopt;
    const auto type = currency.row(static_cast<std::uint64_t>(types[0]));
    if (!type) return std::nullopt;
    return currency.int_field(*type, 4).value_or(0);
}

} // namespace

StarSkillUp upgrade_star_skill(Account& account, const TableBlob* tables, std::int32_t skill_id, std::int32_t target_level) {
    if (!tables || skill_id <= 0 || target_level <= 0) return {};
    const auto current = account.player.star_skills.contains(skill_id) ? account.player.star_skills.at(skill_id) : 0;
    if (target_level <= current) return {.ok = true};
    GameTable skills, item, currency;
    if (!skills.load(*tables, "StarChartsSkill")) return {};
    const auto row = skills.row(static_cast<std::uint64_t>(skill_id));
    if (!row || target_level > skills.int_field(*row, 6).value_or(0)) return {};
    item.load(*tables, "Item");
    currency.load(*tables, "CurrencyType");
    const auto ids = nested_ints(row->bytes, 7);
    const auto nums = nested_ints(row->bytes, 8);
    struct Cost { std::int32_t id; std::int32_t num; std::optional<std::int32_t> enu; };
    std::vector<Cost> costs;
    for (std::int32_t level = current; level < target_level; ++level) {
        if (static_cast<std::size_t>(level) >= ids.size() || static_cast<std::size_t>(level) >= nums.size()) return {};
        const auto& need_ids = ids[static_cast<std::size_t>(level)];
        const auto& need_nums = nums[static_cast<std::size_t>(level)];
        for (std::size_t i = 0; i < need_ids.size(); ++i) {
            costs.push_back(Cost{need_ids[i], i < need_nums.size() ? need_nums[i] : 1, currency_of(item, currency, need_ids[i])});
        }
    }
    for (const auto& cost : costs) {
        if (cost.num <= 0) continue;
        if (cost.enu) {
            const auto* field = wallet(account.player, *cost.enu);
            if (field == nullptr || *field < cost.num) return {};
        } else {
            const auto owned = std::ranges::find(account.items, cost.id, &AccountItem::id);
            if (owned == account.items.end() || owned->num < cost.num) return {};
        }
    }
    for (const auto& cost : costs) {
        if (cost.num <= 0) continue;
        if (cost.enu) *wallet(account.player, *cost.enu) -= cost.num;
        else std::ranges::find(account.items, cost.id, &AccountItem::id)->num -= cost.num;
    }
    account.player.star_skills[skill_id] = target_level;
    return {.ok = true};
}

void finish_guide(Account& account, std::int32_t group, std::int32_t next_group) {
    account.player.guides[group] = 1;
    if (next_group > 0) account.player.guides.try_emplace(next_group, 0);
}

TaskProgress task_progress(const Account& account, const TableBlob& tables, std::int32_t condition_id) {
    GameTable cond;
    if (!cond.load(tables, "TaskCondition")) return {};
    const auto row = cond.row(static_cast<std::uint64_t>(condition_id));
    if (!row) return {};
    const auto type = cond.int_field(*row, 2).value_or(0);
    const auto target = std::max(1, cond.int_field(*row, 5).value_or(1));
    const auto values = cond.ints_field(*row, 3);
    std::int32_t current = 0;
    switch (type) {
        case 3: // E_CustomsPass: completeValue1 is the section ids
            current = static_cast<std::int32_t>(std::ranges::count_if(values, [&](std::int32_t section) {
                return std::ranges::contains(account.player.cleared_main, section);
            }));
            break;
        case 7: // E_HeroLevelUp: levels above 1 are the upgrades still on the roster
            for (const auto& hero : account.heroes) current += std::max(0, hero.level - 1);
            break;
        case 43: // E_AccountLevel
            current = account.player.level;
            break;
        case 44: { // E_HeroLevel / E_HeroSyncStar / E_HeroCount: field 3 为目标星级, field 4 为目标等级
            const auto target_stars = cond.ints_field(*row, 3);
            const auto target_levels = cond.ints_field(*row, 4);
            const auto min_star = target_stars.empty() ? 0 : target_stars.front();
            const auto min_level = target_levels.empty() ? 0 : target_levels.front();
            for (const auto& hero : account.heroes) {
                const bool level_ok = (min_level <= 0) || (hero.level >= min_level);
                const bool star_ok = (min_star <= 0) || (hero.star >= min_star);
                if (level_ok && star_ok) {
                    ++current;
                }
            }
            break;
        }
        case 45: // E_Wish: cards drawn in every pool (a ten-draw is 10)
            for (const auto& [id, pool] : account.player.draw_pools) current += pool.one + 10 * pool.ten;
            break;
        case 49: // E_EquipEquip: pieces actually worn, not sitting in the bag
            for (const auto& hero : account.heroes) current += static_cast<std::int32_t>(hero.worn.size());
            break;
        case 58: // E_LoginDay
            current = account.login_count;
            break;
        default:
            // ponytail: untracked condition types stay at 0 until that action is recorded
            break;
    }
    return {.current = current, .target = target};
}

namespace {

std::int32_t task_status(bool claimed, const TaskProgress& progress) {
    if (claimed) return 4;                         // FINISH
    if (progress.current >= progress.target) return 3; // REWARD
    return 2;                                      // START
}

} // namespace

std::vector<std::uint8_t> challenge_task_reply(const Account& account, const TableBlob& tables) {
    Writer reply;
    reply.int32(1, 10);
    reply.int32(2, 3);
    GameTable tasks;
    if (!tasks.load(tables, "ChallengeTask")) return reply.data();
    std::map<std::int32_t, bool> phase_done;
    for (const auto& row : tasks.rows()) {
        const auto id = tasks.int_field(row, 1).value_or(0);
        if (id <= 0 || tasks.int_field(row, 9).value_or(1) != 1) continue;
        const auto group = tasks.int_field(row, 6).value_or(0);
        const auto claimed = std::ranges::contains(account.player.challenge_tasks, id);
        const auto progress = task_progress(account, tables, tasks.int_field(row, 4).value_or(0));
        phase_done[group] = phase_done.contains(group) ? phase_done[group] && claimed : claimed;
        Writer task;
        task.int32(1, id);
        task.int32(2, task_status(claimed, progress));
        task.int32(3, std::min(progress.current, progress.target));
        reply.message(3, task.data());
    }
    if (GameTable control; control.load(tables, "TaskControl") && !control.rows().empty()) {
        const auto groups = control.ints_field(control.rows().front(), 11);
        for (const auto group : groups) {
            Writer box;
            box.int32(1, group);
            const auto picked = std::ranges::contains(account.player.challenge_boxes, group);
            const auto ready = phase_done.contains(group) && phase_done[group];
            box.int32(2, picked ? 2 : ready ? 1 : 0);
            reply.message(4, box.data());
        }
    }
    return reply.data();
}

namespace {

std::int32_t equip_slot(const TableBlob* tables, std::int32_t type_id) {
    if (!tables) return -1;
    GameTable base;
    if (!base.load(*tables, "EquibBase")) return -1;
    const auto row = base.row(static_cast<std::uint64_t>(type_id));
    if (!row) return -1;
    const auto part = base.int_field(*row, 2).value_or(0) - 1;
    return part >= 0 && part <= 5 ? part : -1;
}

EquipWear heroes_of(const Account& account, const std::vector<std::int32_t>& ids) {
    EquipWear wear{.ok = true};
    for (const auto id : ids) wear.heroes.push_back(*std::ranges::find(account.heroes, id, &AccountHero::id));
    return wear;
}

} // namespace

bool fix_worn_slots(Account& account, const TableBlob& tables) {
    auto changed = false;
    for (auto& hero : account.heroes) {
        std::map<std::int32_t, std::int32_t> fixed;
        for (const auto& [slot, equip_id] : hero.worn) {
            const auto piece = std::ranges::find(account.player.equips, equip_id, &AccountEquip::id);
            if (piece == account.player.equips.end()) continue;
            const auto correct = equip_slot(&tables, piece->type_id);
            if (correct < 0) continue;
            fixed[correct] = equip_id;
            if (correct != slot) changed = true;
        }
        if (fixed.size() != hero.worn.size()) changed = true;
        hero.worn = std::move(fixed);
    }
    return changed;
}



bool roll_equip_minor(AccountEquip& equip, const TableBlob& tables) {
    GameTable base, attrib;
    if (!base.load(tables, "EquibBase") || !attrib.load(tables, "EquibAttrib")) return false;
    const auto brow = base.row(static_cast<std::uint64_t>(equip.type_id));
    if (!brow) return false;
    const auto types = base.ints_field(*brow, 6);
    const auto chances = base.ints_field(*brow, 7);
    static std::mt19937 rng{std::random_device{}()};
    std::int32_t total = 0;
    for (const auto c : chances) total += c;
    if (types.empty() || total <= 0) return false;
    for (std::int32_t attempt = 0; attempt < 32; ++attempt) {
        std::int32_t roll = static_cast<std::int32_t>(rng() % static_cast<std::uint32_t>(total));
        std::size_t idx = 0;
        for (; idx < chances.size(); ++idx) {
            roll -= chances[idx];
            if (roll < 0) break;
        }
        if (idx >= types.size()) idx = types.size() - 1;
        const auto t = types[idx];
        if (t <= 0 || equip.attrs.contains(t)) continue;
        for (const auto& arow : attrib.rows()) {
            if (attrib.int_field(arow, 1).value_or(0) != equip.star) continue;
            if (attrib.int_field(arow, 2).value_or(0) != 1) continue; // AttribSRC 1 = 小词条
            if (attrib.int_field(arow, 3).value_or(0) != t) continue;
            const auto sec = attrib.ints_field(arow, 4);
            if (sec.empty()) break;
            equip.attrs[t] = sec[std::rand() % static_cast<std::int32_t>(sec.size())];
            return true;
        }
    }
    return false;
}

EquipStrengthenResult apply_equip_strengthen(Account& account, const TableBlob* tables, std::int32_t equip_id) {
    auto equip_it = std::ranges::find(account.player.equips, equip_id, &AccountEquip::id);
    if (equip_it == account.player.equips.end()) {
        return EquipStrengthenResult{.ok = false, .code = 27, .equip_id = equip_id}; // 27: E_NO_EQUIP
    }
    if (equip_it->level >= 15) {
        return EquipStrengthenResult{.ok = false, .code = 14, .equip_id = equip_id, .new_level = equip_it->level, .updated_equip = *equip_it}; // 14: E_MAX_LEVEL
    }

    std::int32_t cost_exp = 50;
    std::int32_t cost_gold = 800;

    if (tables) {
        GameTable exp_tbl;
        if (exp_tbl.load(*tables, "EquibExp")) {
            const auto row = exp_tbl.row(static_cast<std::uint64_t>(equip_it->level));
            if (!row) {
                return EquipStrengthenResult{.ok = false, .code = 13, .equip_id = equip_id, .new_level = equip_it->level, .updated_equip = *equip_it}; // 13: E_ERROR_OPT
            }
            const auto need_exp = exp_tbl.int_field(*row, 2).value_or(0);
            const auto need_gold = exp_tbl.int_field(*row, 5).value_or(0);

            std::int32_t exp_bonus = 1000;
            std::int32_t gold_bonus = 1000;
            GameTable stage_tbl;
            if (stage_tbl.load(*tables, "EquibStage")) {
                const auto stage_key = static_cast<std::uint64_t>(std::clamp(equip_it->star, 1, 6));
                if (const auto s_row = stage_tbl.row(stage_key)) {
                    exp_bonus = stage_tbl.int_field(*s_row, 7).value_or(1000);
                    gold_bonus = stage_tbl.int_field(*s_row, 8).value_or(1000);
                }
            }
            cost_exp = (exp_bonus * need_exp) / 1000;
            cost_gold = (gold_bonus * need_gold) / 1000;
        }
    }

    if (account.player.equip_exp < cost_exp) {
        return EquipStrengthenResult{.ok = false, .code = 26, .equip_id = equip_id, .new_level = equip_it->level, .updated_equip = *equip_it}; // 26: E_LIMIT_EQUIP_EXP
    }
    if (account.player.gold < cost_gold) {
        return EquipStrengthenResult{.ok = false, .code = 35, .equip_id = equip_id, .new_level = equip_it->level, .updated_equip = *equip_it}; // 35: E_LIMIT_GOLD
    }

    account.player.equip_exp -= cost_exp;
    account.player.gold -= cost_gold;
    equip_it->level += 1;

    if (tables) {
        GameTable exp_tbl2;
        if (exp_tbl2.load(*tables, "EquibExp")) {
            const auto lv_row = exp_tbl2.row(static_cast<std::uint64_t>(equip_it->level));
            if (lv_row && exp_tbl2.int_field(*lv_row, 7).value_or(0) == 1) { // f7 IsEvent
                GameTable stage_tbl2;
                std::int32_t minor_max = 5;
                if (stage_tbl2.load(*tables, "EquibStage")) {
                    if (const auto s_row = stage_tbl2.row(static_cast<std::uint64_t>(std::clamp(equip_it->star, 1, 6)))) {
                        minor_max = stage_tbl2.int_field(*s_row, 5).value_or(5); // f5 MinorListMax
                    }
                }
                if (static_cast<std::int32_t>(equip_it->attrs.size()) <
                    minor_max + 1) { // +1 = 主属性槽
                    roll_equip_minor(*equip_it, *tables);
                }
            }
        }
    }

    return EquipStrengthenResult{
        .ok = true,
        .code = 10, // 10: E_Ok
        .equip_id = equip_id,
        .new_level = equip_it->level,
        .updated_equip = *equip_it
    };
}

EquipWear wear_equip(Account& account, const TableBlob* tables, std::int32_t equip_id, std::int32_t hero_id) {
    const auto piece = std::ranges::find(account.player.equips, equip_id, &AccountEquip::id);
    if (piece == account.player.equips.end() ||
        std::ranges::find(account.heroes, hero_id, &AccountHero::id) == account.heroes.end()) {
        return {};
    }
    const auto slot = equip_slot(tables, piece->type_id);
    if (slot < 0) return {};
    std::vector<std::int32_t> changed;
    for (auto& other : account.heroes) {
        const auto before = other.worn.size();
        std::erase_if(other.worn, [&](const auto& entry) { return entry.second == equip_id; });
        if (other.worn.size() != before) changed.push_back(other.id);
    }
    auto hero = std::ranges::find(account.heroes, hero_id, &AccountHero::id);
    hero->worn[slot] = equip_id;
    if (!std::ranges::contains(changed, hero_id)) changed.push_back(hero_id);
    return heroes_of(account, changed);
}

EquipWear unequip(Account& account, std::int32_t hero_id, std::int32_t pos) {
    auto hero = std::ranges::find(account.heroes, hero_id, &AccountHero::id);
    if (hero == account.heroes.end()) return {};
    if (pos < 0) hero->worn.clear();
    else hero->worn.erase(pos);
    return heroes_of(account, {hero_id});
}

std::int32_t save_equip_plan(Account& account, std::int32_t id, std::string name,
                             std::map<std::int32_t, std::int32_t> positions) {
    if (id <= 0) {
        id = 1;
        for (const auto& plan : account.player.equip_plans) id = std::max(id, plan.id + 1);
    }
    auto plan = std::ranges::find(account.player.equip_plans, id, &AccountEquipPlan::id);
    if (plan == account.player.equip_plans.end()) {
        account.player.equip_plans.push_back(AccountEquipPlan{.id = id, .name = std::move(name), .positions = std::move(positions)});
    } else {
        plan->name = std::move(name);
        plan->positions = std::move(positions);
    }
    std::erase(account.player.cleared_equip_plans, id);
    return id;
}

bool delete_equip_plan(Account& account, std::int32_t id) {
    if (id < 0 || id >= 0x10000) return false;
    std::erase_if(account.player.equip_plans, [&](const AccountEquipPlan& plan) { return plan.id == id; });
    if (!std::ranges::contains(account.player.cleared_equip_plans, id)) account.player.cleared_equip_plans.push_back(id);
    return true;
}

TaskClaim claim_challenge_task(Account& account, const TableBlob* tables, std::int32_t task_id) {
    if (!tables || task_id <= 0) return {};
    if (std::ranges::contains(account.player.challenge_tasks, task_id)) return {.ok = true, .already = true};
    GameTable tasks, gift;
    if (!tasks.load(*tables, "ChallengeTask")) return {};
    const auto row = tasks.row(static_cast<std::uint64_t>(task_id));
    if (!row) return {};
    const auto progress = task_progress(account, *tables, tasks.int_field(*row, 4).value_or(0));
    if (progress.current < progress.target) return {};
    gift.load(*tables, "Gift");
    std::vector<AccountItem> rewards;
    expand_gift(gift, tasks.int_field(*row, 7).value_or(0), rewards);
    auto paid = pay_rewards(account, tables, rewards);
    TaskClaim claim{.ok = true, .player = paid.player, .shown = std::move(paid.shown), .bag = std::move(paid.bag),
                    .equips = std::move(paid.equips)};
    if (paid.role_exp) claim.level_ups = settle_role_level(account, tables);
    if (claim.level_ups > 0) claim.player = true;
    account.player.challenge_tasks.push_back(task_id);
    return claim;
}

TaskClaim pick_challenge_box(Account& account, const TableBlob* tables, std::int32_t phase) {
    if (!tables || phase <= 0) return {};
    if (std::ranges::contains(account.player.challenge_boxes, phase)) return {.ok = true, .already = true};
    GameTable tasks, control, gift;
    if (!tasks.load(*tables, "ChallengeTask") || !control.load(*tables, "TaskControl") || control.rows().empty()) return {};
    auto any = false;
    for (const auto& row : tasks.rows()) {
        if (tasks.int_field(row, 6).value_or(0) != phase || tasks.int_field(row, 9).value_or(1) != 1) continue;
        any = true;
        const auto id = tasks.int_field(row, 1).value_or(0);
        if (!std::ranges::contains(account.player.challenge_tasks, id)) return {};
    }
    if (!any) return {};
    const auto groups = control.ints_field(control.rows().front(), 11);
    const auto gifts = control.ints_field(control.rows().front(), 12);
    const auto index = std::ranges::find(groups, phase) - groups.begin();
    if (static_cast<std::size_t>(index) >= gifts.size()) return {};
    gift.load(*tables, "Gift");
    std::vector<AccountItem> rewards;
    expand_gift(gift, gifts[static_cast<std::size_t>(index)], rewards);
    auto paid = pay_rewards(account, tables, rewards);
    TaskClaim claim{.ok = true, .player = paid.player, .shown = std::move(paid.shown), .bag = std::move(paid.bag),
                    .equips = std::move(paid.equips)};
    if (paid.role_exp) claim.level_ups = settle_role_level(account, tables);
    if (claim.level_ups > 0) claim.player = true;
    account.player.challenge_boxes.push_back(phase);
    return claim;
}

namespace {

constexpr std::int64_t kServerUtcOffset = 8 * 3600; // task days and hours are Beijing time

// DailyTask.RefreshCycle 1 rows; CompleteNum is at least 1.
struct DailyTaskRow {
    std::int32_t id{};
    std::int32_t cond{};
    std::int32_t num{};
    std::int32_t gift{};
};

std::optional<DailyTaskRow> daily_task_row(const GameTable& tasks, const GameTable& conds,
                                           const TableRow& row, std::int32_t level) {
    if (tasks.int_field(row, 7).value_or(0) != 1 || tasks.int_field(row, 5).value_or(0) > level) return std::nullopt;
    DailyTaskRow task{.id = tasks.int_field(row, 1).value_or(0), .cond = tasks.int_field(row, 4).value_or(0),
                      .gift = tasks.int_field(row, 8).value_or(0)};
    const auto cond = conds.row(static_cast<std::uint64_t>(task.cond));
    if (task.id <= 0 || !cond) return std::nullopt;
    task.num = std::max(1, conds.int_field(*cond, 5).value_or(1));
    return task;
}

// Login and the online window (CompleteValue1..2 hours) are read off the clock
// and stick for the rest of the game day.
std::int32_t task_count(Account& account, const GameTable& conds, std::int32_t cond_id, std::int64_t now) {
    auto& count = account.player.daily_progress[cond_id];
    if (const auto row = conds.row(static_cast<std::uint64_t>(cond_id))) {
        const auto type = conds.int_field(*row, 2).value_or(0);
        const auto from = conds.ints_field(*row, 3);
        const auto to = conds.ints_field(*row, 4);
        const auto hour = (now + kServerUtcOffset) % 86400 / 3600;
        if (type == kTaskSignIn ||
            (type == kTaskOnlineAt && !from.empty() && !to.empty() && hour >= from[0] && hour < to[0])) {
            count = std::max(count, 1);
        }
    }
    return count;
}

} // namespace

void roll_daily_tasks(Account& account, const TableBlob& tables, std::int64_t now) {
    GameTable control;
    const auto refresh = control.load(tables, "TaskControl") && !control.rows().empty()
                             ? control.int_field(control.rows().front(), 2).value_or(0)
                             : 0;
    const auto day = static_cast<std::int32_t>((now + kServerUtcOffset - refresh * 3600) / 86400);
    auto& p = account.player;
    if (p.task_day == day) return;
    if (p.task_day != 0) { // saves from before task_day keep today's claims
        p.daily_progress.clear();
        p.daily_tasks.clear();
        p.daily_boxes.clear();
        p.daily_activity = 0;
        p.shop_refresh_times = 0;
        p.shop_refresh_day = day;
        p.shop_goods_bought.clear();
    }
    p.task_day = day;
}

void count_daily_task(Account& account, const TableBlob& tables, std::int32_t type, std::int32_t value,
                      std::int32_t n) {
    GameTable tasks, conds;
    if (n <= 0 || !tasks.load(tables, "DailyTask") || !conds.load(tables, "TaskCondition")) return;
    roll_daily_tasks(account, tables, std::time(nullptr));
    for (const auto& row : tasks.rows()) {
        const auto task = daily_task_row(tasks, conds, row, std::numeric_limits<std::int32_t>::max());
        if (!task) continue;
        const auto cond = conds.row(static_cast<std::uint64_t>(task->cond));
        if (conds.int_field(*cond, 2).value_or(0) != type) continue;
        const auto values = conds.ints_field(*cond, 3);
        if (!values.empty() && values[0] != 0 && !std::ranges::contains(values, value)) continue;
        auto& count = account.player.daily_progress[task->cond];
        count = clamp_add(count, n);
    }
}

std::vector<std::uint8_t> daily_task_reply(Account& account, const TableBlob& tables) {
    const auto now = std::time(nullptr);
    roll_daily_tasks(account, tables, now);
    Writer reply;
    reply.int32(1, 10);
    reply.int32(2, 1);
    if (GameTable tasks, conds; tasks.load(tables, "DailyTask") && conds.load(tables, "TaskCondition")) {
        for (const auto& row : tasks.rows()) {
            const auto task = daily_task_row(tasks, conds, row, account.player.level);
            if (!task) continue;
            const auto prev = tasks.int_field(row, 6).value_or(0);
            if (prev > 0 && !std::ranges::contains(account.player.daily_tasks, prev)) continue;
            const TaskProgress progress{.current = task_count(account, conds, task->cond, now), .target = task->num};
            Writer data;
            data.int32(1, task->id);
            data.int32(2, task_status(std::ranges::contains(account.player.daily_tasks, task->id), progress));
            data.int32(3, std::min(progress.current, progress.target));
            reply.message(3, data.data());
        }
    }
    if (GameTable control; control.load(tables, "TaskControl") && !control.rows().empty()) {
        const auto marks = control.ints_field(control.rows().front(), 4);
        for (std::size_t i = 0; i < marks.size(); ++i) {
            Writer box;
            box.int32(1, static_cast<std::int32_t>(i));
            const auto picked = std::ranges::contains(account.player.daily_boxes, marks[i]);
            box.int32(2, picked ? 2 : account.player.daily_activity >= marks[i] ? 1 : 0);
            reply.message(4, box.data());
        }
    }
    return reply.data();
}

TaskClaim claim_daily_task(Account& account, const TableBlob* tables, std::int32_t task_id) {
    if (!tables || task_id <= 0) return {};
    const auto now = std::time(nullptr);
    roll_daily_tasks(account, *tables, now);
    if (std::ranges::contains(account.player.daily_tasks, task_id)) return {.ok = true, .already = true};
    GameTable tasks, conds, gift;
    if (!tasks.load(*tables, "DailyTask") || !conds.load(*tables, "TaskCondition")) return {};
    const auto row = tasks.row(static_cast<std::uint64_t>(task_id));
    const auto task = row ? daily_task_row(tasks, conds, *row, account.player.level) : std::nullopt;
    if (!task || task_count(account, conds, task->cond, now) < task->num) return {};
    gift.load(*tables, "Gift");
    std::vector<AccountItem> rewards;
    expand_gift(gift, task->gift, rewards);
    auto paid = pay_rewards(account, tables, rewards);
    TaskClaim claim{.ok = true, .player = paid.player, .shown = std::move(paid.shown), .bag = std::move(paid.bag),
                    .equips = std::move(paid.equips)};
    if (paid.role_exp) claim.level_ups = settle_role_level(account, tables);
    if (claim.level_ups > 0) claim.player = true;
    account.player.daily_tasks.push_back(task_id);
    return claim;
}

TaskClaim pick_daily_box(Account& account, const TableBlob* tables, std::int32_t box_id) {
    if (!tables || box_id < 0) return {};
    roll_daily_tasks(account, *tables, std::time(nullptr));
    GameTable control, gift;
    if (!control.load(*tables, "TaskControl") || control.rows().empty()) return {};
    const auto& row = control.rows().front();
    const auto marks = control.ints_field(row, 4);
    const auto gifts = control.ints_field(row, 5);
    const auto index = static_cast<std::size_t>(box_id); // NormalTaskTab sends the 0-based box index
    if (index >= marks.size() || index >= gifts.size()) return {};
    const auto mark = marks[index];
    if (std::ranges::contains(account.player.daily_boxes, mark)) return {.ok = true, .already = true};
    if (account.player.daily_activity < mark) return {};
    gift.load(*tables, "Gift");
    std::vector<AccountItem> rewards;
    expand_gift(gift, gifts[index], rewards);
    auto paid = pay_rewards(account, tables, rewards);
    TaskClaim claim{.ok = true, .player = paid.player, .shown = std::move(paid.shown), .bag = std::move(paid.bag),
                    .equips = std::move(paid.equips)};
    if (paid.role_exp) claim.level_ups = settle_role_level(account, tables);
    if (claim.level_ups > 0) claim.player = true;
    account.player.daily_boxes.push_back(mark);
    return claim;
}

std::vector<std::uint8_t> checkout_reply(const Account& account, const CheckoutRequest& request,
                                         const CheckoutResult& result) {
    Writer reply;
    reply.int32(1, 10); // result E_Ok
    reply.boolean(2, request.success);
    Writer reward;
    for (const auto& item : result.rewards) {
        Writer entry; // RewardItem: 1 itemId, 2 itemNum
        entry.int32(1, item.id);
        entry.int32(2, item.num);
        reward.message(1, entry.data());
    }
    for (const auto& equip : result.reward_equips) {
        Writer entry; // HeroEquip: 1 id, 2 typeId, 3 level, 5 star
        entry.int32(1, equip.id);
        entry.int32(2, equip.type_id);
        entry.int32(3, equip.level);
        entry.int32(5, equip.star);
        reward.message(2, entry.data());
    }
    reply.message(3, reward.data());
    for (const auto id : request.heroes) {
        const auto owned = std::ranges::find(account.heroes, id, &AccountHero::id);
        const bool has = owned != account.heroes.end();
        reply.int32(4, id);                                            // heroIDList
        reply.int32(5, has ? owned->level : 1);                        // heroLevel
        reply.int32(6, has ? static_cast<std::int32_t>(owned->exp) : 0); // heroExp
        reply.int32(7, 0);                                             // heroUpLevelNum
        reply.int32(11, 0);                                            // heroFavorExp
        reply.int32(12, 0);                                            // heroAddFavorExp
        reply.int32(13, 0);                                            // heroFavorLevel
        reply.boolean(14, false);                                      // heroFullLevel
        reply.boolean(15, false);                                      // favorFullLevel
    }
    reply.int32(8, account.player.level);  // roleLevel
    reply.int32(9, result.level_ups);      // UpLevelNum
    reply.int32(10, account.player.exp);   // roleExp
    reply.int32(20, result.returned_power); // returnPowerValue
    return reply.data();
}

HeroOptRequest parse_hero_opt(std::span<const std::uint8_t> body) {
    HeroOptRequest request;
    Reader reader{body};
    while (!reader.eof()) {
        const auto field = reader.field();
        if (!field) break;
        if (field->wire_type == 0 && field->number >= 1 && field->number <= 3) {
            const auto value = static_cast<std::int32_t>(reader.varint().value_or(0));
            if (field->number == 1) request.id = value;
            else if (field->number == 2) request.opt = value;
            else request.consume_item = value;
        } else if (field->number == 4 && field->wire_type == 2) {
            request.name = reader.string().value_or("");
        } else if (!reader.skip(field->wire_type)) {
            break;
        }
    }
    return request;
}

std::int32_t default_hero_star(const TableBlob* tables, std::int32_t hero_id) {
    if (!tables || hero_id <= 0) return 1;
    GameTable attrib;
    if (!attrib.load(*tables, "PlayerAttrib")) return 1;
    const auto row = attrib.row(static_cast<std::uint64_t>(hero_id));
    if (!row) return 1;
    const auto stage = attrib.int_field(*row, 7).value_or(1);
    return stage > 0 ? stage : 1;
}

std::int32_t max_hero_star(const TableBlob* tables) {
    if (!tables) return std::numeric_limits<std::int32_t>::max();
    GameTable table;
    if (!table.load(*tables, "PlayerStage") || table.rows().empty()) {
        return std::numeric_limits<std::int32_t>::max();
    }
    std::uint64_t max_key = 0;
    for (const auto& row : table.rows()) max_key = std::max(max_key, row.key);
    return static_cast<std::int32_t>(max_key);
}

namespace {

AccountHero& owned_hero(Account& account, const TableBlob* tables, std::int32_t id) {
    const auto it = std::ranges::find(account.heroes, id, &AccountHero::id);
    if (it != account.heroes.end()) return *it;
    return account.heroes.emplace_back(
        AccountHero{.id = id, .state = 2, .level = 1, .star = default_hero_star(tables, id)});
}

std::int32_t chip_item_id(const TableBlob* tables, std::int32_t hero_id, std::int32_t fallback) {
    if (fallback > 0) return fallback;
    if (!tables) return 0;
    GameTable attrib;
    if (!attrib.load(*tables, "PlayerAttrib")) return 0;
    const auto row = attrib.row(static_cast<std::uint64_t>(hero_id));
    return row ? attrib.int_field(*row, 11).value_or(0) : 0;
}

// HeroLevelUP: need = PlayerLevelBonus[hero.level].HeroExp - hero.exp, paid from E_HeroExp.
std::int32_t hero_level_exp(const TableBlob* tables, std::int32_t level) {
    if (!tables || level <= 0) return -1;
    GameTable table;
    if (!table.load(*tables, "PlayerLevelBonus")) return -1;
    const auto row = table.row(static_cast<std::uint64_t>(level));
    return row ? table.int_field(*row, 2).value_or(0) : -1;
}

std::int32_t stage_chip_cost(const TableBlob* tables, std::int32_t stage) {
    if (!tables || stage <= 0) return 0;
    GameTable table;
    if (!table.load(*tables, "PlayerStage")) return 0;
    const auto row = table.row(static_cast<std::uint64_t>(stage));
    return row ? table.int_field(*row, 2).value_or(0) : 0;
}

std::int32_t unlock_chip_cost(const TableBlob* tables, std::int32_t hero_id) {
    if (!tables) return 0;
    GameTable attrib;
    if (!attrib.load(*tables, "PlayerAttrib")) return 0;
    const auto row = attrib.row(static_cast<std::uint64_t>(hero_id));
    const auto last = row ? attrib.int_field(*row, 7).value_or(0) : 0;
    std::int32_t cost = 0;
    for (std::int32_t stage = 1; stage <= last; ++stage) cost += stage_chip_cost(tables, stage);
    return cost;
}

bool take_item(Account& account, std::int32_t id, std::int32_t num, std::vector<AccountItem>& changed) {
    if (id <= 0 || num <= 0) return true;
    auto it = std::ranges::find(account.items, id, &AccountItem::id);
    if (it == account.items.end() || it->num < num) return false;
    it->num -= num;
    changed.push_back(*it);
    if (it->num <= 0) account.items.erase(it);
    return true;
}

} // namespace

HeroOptResult apply_hero_opt(Account& account, const TableBlob* tables, const HeroOptRequest& request) {
    constexpr std::int32_t kNoHero = 20, kNoChip = 21, kNoItem = 22;
    constexpr std::int32_t kUnlock = 0, kUpLevel = 1, kUpStar = 2, kName = 3;
    if (request.id <= 0) return {.code = kNoHero};
    const auto owned = std::ranges::find(account.heroes, request.id, &AccountHero::id);
    if (request.opt == kUnlock) {
        const auto stage = default_hero_star(tables, request.id);
        if (owned != account.heroes.end() && owned->state == 2) {
            if (owned->star < stage) owned->star = stage;
            return {.heroes = {*owned}};
        }
        const auto chip = chip_item_id(tables, request.id, request.consume_item);
        const auto cost = unlock_chip_cost(tables, request.id);
        HeroOptResult result;
        if (!take_item(account, chip, cost, result.items)) return {.code = kNoChip};
        auto& hero = owned_hero(account, tables, request.id);
        hero.state = 2;
        if (hero.star < stage) hero.star = stage;
        result.heroes.push_back(hero);
        // 新获得英雄的默认头像在 PlayerData IconInfo.f4 — 置 player 触发重推,
        // 头像列表实时出现 (与升星跨 5 星阈值同机制)
        result.player = true;
        return result;
    }
    if (owned == account.heroes.end()) return {.code = kNoHero};
    if (request.opt == kUpLevel) {
        if (owned->level >= account.player.level) return {.code = 13};
        const auto need = hero_level_exp(tables, owned->level);
        if (need < 0) return {.code = 13};
        const auto cost = std::max(0, need - static_cast<std::int32_t>(owned->exp));
        if (account.player.hero_exp < cost) return {.code = kNoItem};
        account.player.hero_exp -= cost;
        ++owned->level;
        owned->exp = 0;
        if (tables) count_daily_task(account, *tables, kTaskHeroLevelUp, 0);
        return {.heroes = {*owned}, .player = true};
    }
    if (request.opt == kUpStar) {
        if (owned->star >= max_hero_star(tables)) return {.code = 13};
        const auto chip = chip_item_id(tables, request.id, request.consume_item);
        const auto cost = stage_chip_cost(tables, owned->star + 1);
        HeroOptResult result;
        if (!take_item(account, chip, cost, result.items)) return {.code = cost > 0 ? kNoChip : kNoItem};
        const auto old_stage = owned->star;
        ++owned->star;
        // 5 星阶段末解锁的是"战斗中获得月光系神迹 → 技能进化"的档位资格
        // (SkillUpID 链 = 战斗内神迹强化的档), 由客户端按 HeroData.star 在战斗内
        // 处理 — 不动拓本等级 (hero.skills), 两套强化正交。
        result.heroes.push_back(*owned);
        // 跨 5 星阈值 (PlayerStage.BigStarNum=5 的首 stage 11) 解锁 E_Stage 觉醒皮肤 +
        // 星级头像: 皮肤走 571 (router), 头像在 PlayerData IconInfo.f4 — 必须重推
        // PlayerData 才能实时出现在头像列表, 否则要重登才可见。
        if (old_stage < 11 && owned->star >= 11) result.player = true;
        return result;
    }
    if (request.opt == kName) {
        owned->name = request.name;
        return {.heroes = {*owned}};
    }
    return {.code = 13}; // E_ERROR_OPT
}

// SkillBase.pos E_GodSpell, skillUnlockType E_ItemUnlock: [PlayerStage key, item, count].
struct GodSpellCost {
    std::int32_t stage{};
    std::int32_t item_id{};
    std::int32_t count{};
};

std::optional<GodSpellCost> god_spell_cost(const TableBlob* tables, std::int32_t hero_id) {
    if (!tables) return std::nullopt;
    GameTable unit;
    GameTable skill;
    if (!unit.load(*tables, "UnitBase") || !skill.load(*tables, "SkillBase")) return std::nullopt;
    const auto row = unit.row(static_cast<std::uint64_t>(hero_id));
    if (!row) return std::nullopt;
    for (const auto skill_id : unit.ints_field(*row, 22)) {
        const auto skill_row = skill.row(static_cast<std::uint64_t>(skill_id));
        if (!skill_row || skill.int_field(*skill_row, 13).value_or(0) != 8) continue;
        const auto unlock = skill.ints_field(*skill_row, 15);
        if (unlock.size() < 3 || unlock[1] <= 0 || unlock[2] <= 0) return std::nullopt;
        return GodSpellCost{unlock[0], unlock[1], unlock[2]};
    }
    return std::nullopt;
}


HeroOptResult apply_god_like(Account& account, const TableBlob* tables, std::int32_t hero_id) {
    constexpr std::int32_t kNoHero = 20, kNoItem = 22;
    const auto owned = std::ranges::find(account.heroes, hero_id, &AccountHero::id);
    if (owned == account.heroes.end()) return {.code = kNoHero};
    if (owned->god_shed) return {.heroes = {*owned}};
    const auto cost = god_spell_cost(tables, hero_id);
    if (!cost || owned->star < cost->stage) return {.code = 13};
    HeroOptResult result;
    if (!take_item(account, cost->item_id, cost->count, result.items)) return {.code = kNoItem};
    owned->god_shed = true;
    result.heroes.push_back(*owned);
    return result;
}

HeroOptResult apply_god_slot(Account& account, const TableBlob* tables, std::int32_t hero_id, std::int32_t slot) {
    constexpr std::int32_t kNoHero = 20, kNoItem = 22;
    const auto owned = std::ranges::find(account.heroes, hero_id, &AccountHero::id);
    if (owned == account.heroes.end()) return {.code = kNoHero};
    if (!owned->god_shed || slot <= 0) return {.code = 13};
    if (std::ranges::find(owned->god_slots, slot) != owned->god_slots.end()) return {.heroes = {*owned}};
    if (!tables) return {.code = 13};
    GameTable holes;
    if (!holes.load(*tables, "GodHole")) return {.code = 13};
    const auto key = (static_cast<std::uint64_t>(slot) << 32) | static_cast<std::uint64_t>(hero_id);
    const auto row = holes.row(key);
    if (!row) return {.code = 13};
    const auto item = holes.int_field(*row, 4).value_or(0);
    const auto cost = holes.int_field(*row, 5).value_or(0);
    HeroOptResult result;
    if (!take_item(account, item, cost, result.items)) return {.code = kNoItem};
    owned->god_slots.push_back(slot);
    result.heroes.push_back(*owned);
    return result;
}

HeroOptResult apply_hero_skill_up(Account& account, const TableBlob* tables,
                                  std::int32_t hero_id, std::int32_t skill_id,
                                  std::int32_t uplevel) {
    constexpr std::int32_t kOk = 10;
    constexpr std::int32_t kErrOpt = 13;
    constexpr std::int32_t kMaxLevel = 14;
    constexpr std::int32_t kNoHero = 20;
    constexpr std::int32_t kNoItem = 22;
    constexpr std::int32_t kNoSkill = 32;
    constexpr std::int32_t kLimitHeroLevel = 33;
    constexpr std::int32_t kLimitHeroStar = 34;
    constexpr std::int32_t kLimitGold = 35;

    if (hero_id <= 0 || skill_id <= 0) return {.code = kErrOpt};
    const auto owned = std::ranges::find(account.heroes, hero_id, &AccountHero::id);
    if (owned == account.heroes.end()) return {.code = kNoHero};

    if (uplevel <= 0) uplevel = 1;
    if (!tables) return {.code = kErrOpt};

    // Verify skill belongs to this hero if UnitBase is available
    GameTable unit;
    if (unit.load(*tables, "UnitBase")) {
        if (const auto unit_row = unit.row(static_cast<std::uint64_t>(hero_id))) {
            const auto hero_skills = unit.ints_field(*unit_row, 22);
            if (!hero_skills.empty() && std::ranges::find(hero_skills, skill_id) == hero_skills.end()) {
                return {.code = kNoSkill};
            }
        }
    }

    GameTable skill_levels;
    if (!skill_levels.load(*tables, "SkillLevel")) return {.code = kErrOpt};

    std::int32_t current_level = owned->skills.contains(skill_id) ? owned->skills.at(skill_id) : 1;
    if (current_level <= 0) current_level = 1;

    std::map<std::int32_t, std::int32_t> total_items_needed;
    std::int64_t total_gold_needed = 0;

    for (std::int32_t step = 0; step < uplevel; ++step) {
        const std::int32_t lvl = current_level + step;
        const auto key = (static_cast<std::uint64_t>(lvl) << 32) | static_cast<std::uint64_t>(skill_id);
        const auto row = skill_levels.row(key);
        if (!row) return {.code = kMaxLevel};

        const auto grow_cond_type = skill_levels.int_field(*row, 3).value_or(0);
        const auto grow_cond_val = skill_levels.int_field(*row, 4).value_or(0);
        if (grow_cond_type == 0) {
            // Cannot grow / max level reached
            return {.code = kMaxLevel};
        }
        if (grow_cond_type == 1) { // E_HeroLevel
            if (owned->level < grow_cond_val) return {.code = kLimitHeroLevel};
        } else if (grow_cond_type == 2) { // E_HeroStage (Star)
            if (owned->star < grow_cond_val) return {.code = kLimitHeroStar};
        }

        // Pre-skills check (field 8: PreSkillID, field 9: PreSkillLevel)
        const auto pre_skills = skill_levels.ints_field(*row, 8);
        const auto pre_levels = skill_levels.ints_field(*row, 9);
        const auto pre_count = std::min(pre_skills.size(), pre_levels.size());
        for (std::size_t p = 0; p < pre_count; ++p) {
            const auto p_id = pre_skills[p];
            const auto p_req_lvl = pre_levels[p];
            if (p_id > 0 && p_req_lvl > 0) {
                const auto p_cur_lvl = owned->skills.contains(p_id) ? owned->skills.at(p_id) : 1;
                if (p_cur_lvl < p_req_lvl) return {.code = kErrOpt};
            }
        }

        // Gold check (field 7: GoldConsume)
        const auto gold_cost = skill_levels.int_field(*row, 7).value_or(0);
        if (gold_cost > 0) {
            total_gold_needed += gold_cost;
        }

        // Items check (field 5: Item, field 6: ItemNum)
        const auto req_items = skill_levels.ints_field(*row, 5);
        const auto req_nums = skill_levels.ints_field(*row, 6);
        const auto item_count = std::min(req_items.size(), req_nums.size());
        for (std::size_t it = 0; it < item_count; ++it) {
            const auto item_id = req_items[it];
            const auto count = req_nums[it];
            if (item_id > 0 && count > 0) {
                total_items_needed[item_id] += count;
            }
        }
    }

    // Check wallet gold balance
    if (total_gold_needed > 0 && account.player.gold < total_gold_needed) {
        return {.code = kLimitGold};
    }

    // Check item balances
    for (const auto& [item_id, count] : total_items_needed) {
        const auto found = std::ranges::find(account.items, item_id, &AccountItem::id);
        if (found == account.items.end() || found->num < count) {
            return {.code = kNoItem};
        }
    }

    // Deduct
    HeroOptResult result;
    if (total_gold_needed > 0) {
        account.player.gold -= total_gold_needed;
        result.player = true;
    }
    for (const auto& [item_id, count] : total_items_needed) {
        take_item(account, item_id, count, result.items);
    }

    owned->skills[skill_id] = current_level + uplevel;
    result.heroes.push_back(*owned);
    result.code = kOk;
    return result;
}

namespace {

struct Ymd {
    int year{};
    int month{};
    int day{};
    int mmdd{};
    int doy{};
};

Ymd ymd_of(std::int32_t now) {
    std::tm tm{};
    const auto t = static_cast<std::time_t>(now);
    localtime_r(&t, &tm);
    static constexpr int kDays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    Ymd y{.year = tm.tm_year + 1900, .month = tm.tm_mon + 1, .day = tm.tm_mday};
    y.mmdd = y.month * 100 + y.day;
    y.doy = y.day;
    for (int month = 1; month < y.month; ++month) y.doy += kDays[month];
    return y;
}

bool same_game_day(std::int64_t a, std::int64_t b, std::int32_t refresh_hour = 5) {
    if (a <= 0 || b <= 0) return false;
    constexpr std::int64_t kServerUtcOffset = 8 * 3600; // task days and hours are Beijing time
    const auto day_a = (a + kServerUtcOffset - refresh_hour * 3600) / 86400;
    const auto day_b = (b + kServerUtcOffset - refresh_hour * 3600) / 86400;
    return day_a == day_b;
}

bool same_day(std::int32_t a, std::int32_t b) {
    return same_game_day(a, b, 5);
}

int day_of_year(int month, int day) {
    static constexpr int kDays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    auto n = day;
    for (int cursor = 1; cursor < month; ++cursor) n += kDays[cursor];
    return n;
}

// Constellation.field 3 is the start MMDD, field 4 is the day count.
std::int32_t constellation_stamp(const TableBlob* tables, const Ymd& today) {
    if (!tables) return today.mmdd;
    GameTable stars;
    if (!stars.load(*tables, "Constellation")) return today.mmdd;
    for (const auto& row : stars.rows()) {
        const auto start = stars.int_field(row, 3).value_or(0);
        const auto days = stars.int_field(row, 4).value_or(0);
        if (start <= 0 || days <= 0) continue;
        const auto began = day_of_year(start / 100, start % 100);
        auto cursor = today.doy;
        if (cursor < began) cursor += 365;
        if (cursor >= began && cursor < began + days) return start;
    }
    return today.mmdd;
}

std::int32_t month_gift(const TableBlob& tables, int month, int day) {
    GameTable gifts;
    if (!gifts.load(tables, "ConstellationGift") || gifts.rows().empty()) return 0;
    const auto list = gifts.ints_field(gifts.rows().front(), static_cast<std::uint32_t>(4 + (month - 1) * 2));
    const auto index = static_cast<std::size_t>(day - 1);
    return index < list.size() ? list[index] : 0;
}

} // namespace

TaskClaim claim_rookie_sign(Account& account, const TableBlob* tables, std::int32_t index, std::int32_t now) {
    if (!tables || account.player.level < 7 || index != account.player.sign_count) return {};
    if (same_day(account.player.last_sign_time, now)) return {.already = true};
    GameTable activity, gift;
    if (!activity.load(*tables, "GameActivity")) return {};
    const auto row = activity.row(999999);
    if (!row) return {};
    const auto gifts = activity.ints_field(*row, 4);
    if (index < 0 || static_cast<std::size_t>(index) >= gifts.size()) return {};
    gift.load(*tables, "Gift");
    std::vector<AccountItem> rewards;
    expand_gift(gift, gifts[static_cast<std::size_t>(index)], rewards);
    auto paid = pay_rewards(account, tables, rewards);
    if (account.player.sign_start == 0) account.player.sign_start = now;
    account.player.last_sign_time = now;
    ++account.player.sign_count;
    TaskClaim claim{.ok = true, .player = true, .shown = std::move(paid.shown), .bag = std::move(paid.bag),
                    .equips = std::move(paid.equips)};
    if (paid.role_exp) claim.level_ups = settle_role_level(account, tables);
    return claim;
}

TaskClaim claim_divination(Account& account, const TableBlob* tables, std::int32_t now) {
    if (!tables || account.player.level < 8) return {};
    const auto today = ymd_of(now);
    if (account.player.divination_stamp / 100 != today.mmdd / 100) {
        account.player.divination_check = 0;
        account.player.divination_stamp = today.mmdd;
    }
    const auto bit = 1 << (today.day - 1);
    if ((account.player.divination_check & bit) != 0) return {.already = true};
    const auto group = month_gift(*tables, today.month, today.day);
    if (group <= 0) return {};
    GameTable gift;
    gift.load(*tables, "Gift");
    std::vector<AccountItem> rewards;
    expand_gift(gift, group, rewards);
    auto paid = pay_rewards(account, tables, rewards);
    account.player.divination_check |= bit;
    account.player.divination_stamp = today.mmdd;
    account.player.divination_time = now;
    TaskClaim claim{.ok = true, .player = paid.player, .shown = std::move(paid.shown), .bag = std::move(paid.bag),
                    .equips = std::move(paid.equips)};
    if (paid.role_exp) claim.level_ups = settle_role_level(account, tables);
    return claim;
}

DivinationView divination_view(Account& account, const TableBlob* tables, std::int32_t now) {
    const auto today = ymd_of(now);
    if (account.player.divination_stamp / 100 != today.mmdd / 100) account.player.divination_check = 0;
    account.player.divination_stamp = today.mmdd;
    return {.id = constellation_stamp(tables, today), .check_in = account.player.divination_check, .valid = today.mmdd};
}

TaskClaim use_item(Account& account, const TableBlob* tables, std::int32_t item_id, std::int32_t count,
                   std::vector<std::int32_t> picks) {
    if (!tables || count <= 0) return {};
    GameTable item, gift;
    if (!item.load(*tables, "Item") || !gift.load(*tables, "Gift")) return {};
    const auto row = item.row(static_cast<std::uint64_t>(item_id));
    if (!row) return {};
    const auto groups = item.ints_field(*row, 16); 
    const auto owned = std::ranges::find(account.items, item_id, &AccountItem::id);
    if (groups.empty() || owned == account.items.end() || owned->num < count) return {};
    std::ranges::sort(picks);
    picks.erase(std::ranges::unique(picks).begin(), picks.end());
    std::vector<AccountItem> rewards;
    for (const auto group : groups) {
        const auto grow = gift.row(static_cast<std::uint64_t>(group));
        if (grow && gift.int_field(*grow, 2).value_or(1) == 4) {
            const auto items = gift.ints_field(*grow, 4);
            const auto nums = gift.ints_field(*grow, 5);
            const auto limit = gift.ints_field(*grow, 3);
            if (picks.empty() || picks.front() < 0 || picks.back() >= std::ssize(items) ||
                std::ssize(picks) > (limit.empty() ? 1 : limit[0]))
                return {};
            for (const auto i : picks)
                rewards.push_back({.id = items[i], .num = (i < std::ssize(nums) ? nums[i] : 1) * count});
            continue;
        }
        for (std::int32_t n = 0; n < count; ++n) expand_gift(gift, group, rewards);
    }
    std::vector<AccountItem> merged;
    for (const auto& reward : rewards) {
        if (reward.num <= 0) continue; 
        const auto it = std::ranges::find(merged, reward.id, &AccountItem::id);
        if (it == merged.end()) merged.push_back(reward);
        else it->num = clamp_add(it->num, reward.num);
    }
    std::vector<AccountItem> consumed;
    take_item(account, item_id, count, consumed);
    auto paid = pay_rewards(account, tables, merged);
    TaskClaim claim{.ok = true, .player = paid.player, .shown = std::move(paid.shown), .bag = std::move(consumed),
                    .equips = std::move(paid.equips)};
    claim.bag.insert(claim.bag.end(), paid.bag.begin(), paid.bag.end());
    if (paid.role_exp) claim.level_ups = settle_role_level(account, tables);
    return claim;
}

LuckDraw luck_draw(Account& account, const TableBlob* tables, std::int32_t pool_id, std::int32_t draw_type) {
    if (!tables || !std::ranges::contains(kOpenPools, pool_id) || (draw_type != 0 && draw_type != 1)) return {};
    GameTable draw, currency, gift, item, attrib;
    if (!draw.load(*tables, "DrawParam") || !currency.load(*tables, "CurrencyType") || !gift.load(*tables, "Gift") ||
        !item.load(*tables, "Item") || !attrib.load(*tables, "PlayerAttrib"))
        return {};
    const auto row = draw.row(static_cast<std::uint64_t>(pool_id));
    if (!row) return {};
    const std::int32_t n = draw_type == 1 ? 10 : 1;
    auto& state = account.player.draw_pools[pool_id];
    if (draw.int_field(*row, 2).value_or(0) == 3) {
        const auto limit = draw.ints_field(*row, 3);
        if (limit.size() >= 2 && limit[1] - 10 * state.ten - state.one < n) return {};
    }
    struct Cost { std::int32_t* field; std::int32_t each; std::int32_t take; };
    std::vector<Cost> costs;
    for (const std::uint32_t f : {4u, 5u, 6u}) {
        const auto pair = draw.ints_field(*row, f);
        if (pair.size() < 2 || pair[1] <= 0) continue;
        const auto type = currency.row(static_cast<std::uint64_t>(pair[0]));
        if (auto* field = type ? wallet(account.player, currency.int_field(*type, 4).value_or(-1)) : nullptr)
            costs.push_back(Cost{field, pair[1], 0});

    }
    auto left = n;
    for (auto& cost : costs) {
        cost.take = std::min(left, *cost.field / cost.each);
        left -= cost.take;
    }
    if (left > 0) return {};
    for (const auto& cost : costs) *cost.field -= cost.take * cost.each;
    ++(draw_type == 1 ? state.ten : state.one);

    std::vector<AccountItem> rolled_out;
    const auto hero_star = [&](std::int32_t id) {
        const auto irow = item.row(static_cast<std::uint64_t>(id));
        return irow && item.int_field(*irow, 21).value_or(0) == 4 ? item.int_field(*irow, 7).value_or(0) - 3 : 0;
    };
    constexpr double kNormalStarRates[] = {0.064801, 0.052191, 0.027641};
    const auto roll_common = [&](std::int32_t group) {
        const auto grow = gift.row(static_cast<std::uint64_t>(group));
        if (pool_id != 22203 || !grow) return expand_gift(gift, group, rolled_out);
        const auto items = gift.ints_field(*grow, 4);
        const auto nums = gift.ints_field(*grow, 5);
        auto weights = gift.ints_field(*grow, 3);
        weights.resize(items.size(), 0);
        auto r = std::uniform_real_distribution<double>{}(rng());
        std::int32_t star = 0;
        for (std::int32_t s = 1; s <= 3 && star == 0; ++s)
            if ((r -= kNormalStarRates[s - 1]) < 0) star = s;
        std::vector<double> in_star(items.size());
        for (std::size_t i = 0; i < items.size(); ++i) in_star[i] = hero_star(items[i]) == star ? weights[i] : 0;
        if (std::ranges::all_of(in_star, [](double w) { return w <= 0; })) return expand_gift(gift, group, rolled_out);
        const auto i = std::discrete_distribution<std::size_t>(in_star.begin(), in_star.end())(rng());
        rolled_out.push_back({.id = items[i], .num = i < nums.size() ? nums[i] : 1});
    };
    const auto security = draw.int_field(*row, 11).value_or(0); // SecurityNum -> SecurityPrize(26)
    const auto top = draw.int_field(*row, 12).value_or(0);      // ThreeStarSecurityNum -> TopPrize(27)
    for (std::int32_t i = 0; i < n; ++i) {
        ++state.since_security;
        ++state.since_top;
        if (top > 0 && state.since_top >= top) {
            expand_gift(gift, draw.int_field(*row, 27).value_or(0), rolled_out);
        } else if (security > 0 && state.since_security >= security) {
            expand_gift(gift, draw.int_field(*row, 26).value_or(0), rolled_out);
        } else {
            roll_common(draw.int_field(*row, 28).value_or(0)); // CommonPrize
        }
        const auto star = rolled_out.empty() ? 0 : hero_star(rolled_out.back().id);
        if (star >= 2) state.since_security = 0;
        if (star >= 3) state.since_top = 0;
    }
    const auto& rolled = rolled_out;
    const auto now = static_cast<std::int64_t>(std::time(nullptr));

    LuckDraw result{.ok = true};
    std::vector<AccountItem> grants;
    for (const auto& reward : rolled) {
        const auto irow = item.row(static_cast<std::uint64_t>(reward.id));
        const auto eff = irow ? item.ints_field(*irow, 22) : std::vector<std::int32_t>{};
        if (!irow || item.int_field(*irow, 21).value_or(0) != 4 || eff.empty()) {
            result.cards.push_back({.item_id = reward.id, .num = reward.num});
            state.history.push_back({.item_id = reward.id, .num = reward.num, .time = now});
            grants.push_back(reward);
            continue;
        }
        const auto hero_id = eff[0];
        const auto arow = attrib.row(static_cast<std::uint64_t>(hero_id));
        for (std::int32_t k = 0; k < reward.num; ++k) {
            const bool owned = std::ranges::contains(account.heroes, hero_id, &AccountHero::id);
            result.cards.push_back({.item_id = reward.id, .num = 1, .hero_id = hero_id, .transform = owned});
            state.history.push_back({.item_id = reward.id, .num = 1, .trans = owned, .time = now});
            if (!owned) {
                account.heroes.push_back(
                    AccountHero{.id = hero_id, .state = 2, .level = 1, .star = default_hero_star(tables, hero_id)});
                result.heroes.push_back(account.heroes.back());
            } else if (arow) {
                grants.push_back({.id = attrib.int_field(*arow, 11).value_or(0), .num = attrib.int_field(*arow, 8).value_or(0)});
            }
            if (const auto comp = arow ? attrib.ints_field(*arow, 9) : std::vector<std::int32_t>{}; comp.size() >= 2) {
                result.cards.push_back({.item_id = comp[0], .num = comp[1]});
                grants.push_back({.id = comp[0], .num = comp[1]});
            }
        }
    }
    auto paid = pay_rewards(account, tables, grants);
    result.bag = std::move(paid.bag);
    if (paid.role_exp) result.level_ups = settle_role_level(account, tables);
    return result;
}

TaskClaim claim_course(Account& account, const TableBlob* tables, const std::vector<std::int32_t>& levels) {
    if (!tables) return {};
    GameTable activity, gift;
    if (!activity.load(*tables, "GameActivity") || !gift.load(*tables, "Gift")) return {};
    const auto row = activity.row(kCourseActivity);
    if (!row) return {};
    const auto gates = activity.ints_field(*row, 4);  // RegistrationGift
    const auto free = nested_ints(row->bytes, 6);     // FreeGiftGroup, one gift list per gate
    auto& taken = account.player.course_levels;
    std::vector<AccountItem> rewards;
    bool any = false;
    for (const auto level : levels) {
        const auto at = std::ranges::find(gates, level);
        if (at == gates.end() || level > account.player.level || std::ranges::contains(taken, level)) continue;
        const auto index = static_cast<std::size_t>(at - gates.begin());
        if (index < free.size())
            for (const auto group : free[index]) expand_gift(gift, group, rewards);
        taken.push_back(level);
        any = true;
    }
    if (!any) return {};
    auto paid = pay_rewards(account, tables, rewards);
    TaskClaim claim{.ok = true, .player = paid.player, .shown = std::move(paid.shown), .bag = std::move(paid.bag),
                    .equips = std::move(paid.equips)};
    if (paid.role_exp) claim.level_ups = settle_role_level(account, tables);
    return claim;
}

ArtifactResult handle_artifact_opt(Account& account, const TableBlob* tables,
                                   std::int32_t opt, std::int32_t hero_id,
                                   std::int32_t jewel_id, std::int32_t hole_id) {
    if (!tables) return {.code = 1};
    auto hero_it = std::ranges::find(account.heroes, hero_id, &AccountHero::id);
    if (hero_it == account.heroes.end()) return {.code = 2};

    GameTable attrib_tbl;
    if (!attrib_tbl.load(*tables, "PlayerAttrib")) return {.code = 3};
    const auto attrib_row = attrib_tbl.row(static_cast<std::uint64_t>(hero_id));
    if (!attrib_row) return {.code = 4};

    const std::int32_t profession = attrib_tbl.int_field(*attrib_row, 2).value_or(301);
    const std::int32_t weapon_id = attrib_tbl.int_field(*attrib_row, 13).value_or(0);
    if (hero_it->artifact.id == 0 && weapon_id > 0) {
        hero_it->artifact.id = weapon_id;
    }

    GameTable fuse_tbl;
    if (!fuse_tbl.load(*tables, "ArtifactFuse")) return {.code = 5};

    const std::uint64_t fuse_key = (static_cast<std::uint64_t>(profession) << 32) |
                                   static_cast<std::uint32_t>(hero_it->artifact.star);
    const auto fuse_row = fuse_tbl.row(fuse_key);
    if (!fuse_row) return {.code = 6};

    ArtifactResult res;
    // opt: 0 = JO_LevelUP, 1 = JO_Fuse (突破 / 解锁到1星), 2 = JO_Beset (宝石镶嵌)
    if (opt == 0) { // JO_LevelUP
        if (hero_it->artifact.star == 0) {
            if (hero_it->artifact.level < 100) {
                // 0星充能阶段: 进度增加 fuseValue (100)，达到 100%
                const std::int32_t fuse_val = fuse_tbl.int_field(*fuse_row, 8).value_or(100);
                hero_it->artifact.level = std::min(100, hero_it->artifact.level + fuse_val);
            } else if (account.player.gold >= 20000) {
                account.player.gold -= 20000;
                hero_it->artifact.star = 1;
                hero_it->artifact.level = 0;
                res.player_currency_changed = true;
            }
            res.ok = true;
            res.heroes = {*hero_it};
            return res;
        }

        // star >= 1
        if (hero_it->artifact.level >= 100) return {.code = 7};
        const std::int32_t gold_need = fuse_tbl.int_field(*fuse_row, 9).value_or(0);
        const auto req_items = fuse_tbl.ints_field(*fuse_row, 6);
        const auto req_item_nums = fuse_tbl.ints_field(*fuse_row, 7);

        if (gold_need > 0 && account.player.gold < gold_need) return {.code = 8};

        for (std::size_t i = 0; i < req_items.size() && i < req_item_nums.size(); ++i) {
            const auto req_id = req_items[i];
            const auto req_num = req_item_nums[i];
            if (req_id <= 0 || req_num <= 0) continue;
            const auto it = std::ranges::find(account.items, req_id, &AccountItem::id);
            if (it == account.items.end() || it->num < req_num) return {.code = 9};
        }

        if (gold_need > 0) {
            account.player.gold -= gold_need;
            res.player_currency_changed = true;
        }
        for (std::size_t i = 0; i < req_items.size() && i < req_item_nums.size(); ++i) {
            const auto req_id = req_items[i];
            const auto req_num = req_item_nums[i];
            if (req_id <= 0 || req_num <= 0) continue;
            auto it = std::ranges::find(account.items, req_id, &AccountItem::id);
            if (it != account.items.end()) {
                it->num -= req_num;
                res.items.push_back(*it);
                if (it->num <= 0) account.items.erase(it);
            }
        }

        const std::int32_t fuse_val = fuse_tbl.int_field(*fuse_row, 8).value_or(10);
        hero_it->artifact.level = std::min(100, hero_it->artifact.level + fuse_val);
        res.ok = true;
        res.heroes = {*hero_it};
        return res;

    } else if (opt == 1) { // JO_Fuse (突破 / 升星)
        if (hero_it->artifact.star >= 6) return {.code = 10};

        if (hero_it->artifact.star == 0) {
            // 0星突破至1星
            const std::int32_t gold_need = fuse_tbl.int_field(*fuse_row, 12).value_or(20000);
            if (account.player.gold < gold_need) return {.code = 12};
            account.player.gold -= gold_need;
            res.player_currency_changed = true;
            hero_it->artifact.star = 1;
            hero_it->artifact.level = 0;
            res.ok = true;
            res.heroes = {*hero_it};
            return res;
        }

        // star >= 1
        if (hero_it->artifact.level < 100) return {.code = 11};
        const std::int32_t gold_need = fuse_tbl.int_field(*fuse_row, 12).value_or(0);
        const auto req_items = fuse_tbl.ints_field(*fuse_row, 10);
        const auto req_item_nums = fuse_tbl.ints_field(*fuse_row, 11);

        if (gold_need > 0 && account.player.gold < gold_need) return {.code = 12};

        for (std::size_t i = 0; i < req_items.size() && i < req_item_nums.size(); ++i) {
            const auto req_id = req_items[i];
            const auto req_num = req_item_nums[i];
            if (req_id <= 0 || req_num <= 0) continue;
            const auto it = std::ranges::find(account.items, req_id, &AccountItem::id);
            if (it == account.items.end() || it->num < req_num) return {.code = 13};
        }

        if (gold_need > 0) {
            account.player.gold -= gold_need;
            res.player_currency_changed = true;
        }
        for (std::size_t i = 0; i < req_items.size() && i < req_item_nums.size(); ++i) {
            const auto req_id = req_items[i];
            const auto req_num = req_item_nums[i];
            if (req_id <= 0 || req_num <= 0) continue;
            auto it = std::ranges::find(account.items, req_id, &AccountItem::id);
            if (it != account.items.end()) {
                it->num -= req_num;
                res.items.push_back(*it);
                if (it->num <= 0) account.items.erase(it);
            }
        }

        hero_it->artifact.star += 1;
        hero_it->artifact.level = 0;
        res.ok = true;
        res.heroes = {*hero_it};
        return res;

    } else if (opt == 2) { // JO_Beset (宝石镶嵌/卸下)
        if (hole_id < 0 || hole_id >= 5) return {.code = 14};

        if (jewel_id > 0) {
            hero_it->artifact.jewels[hole_id] = jewel_id;
        } else {
            hero_it->artifact.jewels.erase(hole_id);
        }
        res.ok = true;
        res.heroes = {*hero_it};
        return res;
    }

    return {.code = 15};
}

AddFavorResult apply_add_favor(Account& account, const TableBlob* tables, const AddFavorRequest& request) {
    std::int32_t target_hero = request.hero_id;
    std::int32_t request_num = request.num;
    std::int32_t exp_per_action = 50;
    std::int32_t new_item_count = 0;

    if (request.opt == 1) { // 触摸角色互动 (Touch Interaction, FavorModule.AddAnimFavor)
        std::int32_t fav_num = 5;
        if (tables && request.option_id > 0) {
            GameTable anim_status;
            if (anim_status.load(*tables, "ExpressionAnimStatus")) {
                if (const auto row = anim_status.row(static_cast<std::uint64_t>(request.option_id))) {
                    if (target_hero <= 0) target_hero = anim_status.int_field(*row, 2).value_or(0); // f2 HeroID
                    fav_num = anim_status.int_field(*row, 17).value_or(5); // f17 FavorableNum
                }
            }
        }
        if (target_hero <= 0) {
            target_hero = account.heroes.empty() ? 1003 : account.heroes.front().id;
        }
        request_num = 1;
        exp_per_action = fav_num > 0 ? fav_num : 5;
        ++account.player.stat_talk_given; // 成就: 累计与神格交谈 (620270)

        // FavorabilityDailyLimit=15
        // FavorabilityHeroDailyLimit=5 
        if (tables) {
            // GlobalParamString is a string-key table: interleaved field1 = key
            // name (string), field2 = row{1: key, 2: value} — GameTable (varint
            // keys) cannot load it, so parse the stream directly.
            bool limits_valid = false;
            int daily_limit = 15, hero_limit = 5;
            if (const auto* gp_entry = tables->find("GlobalParamString")) {
                // skip the 4-byte capacity header
                proto::Reader reader{std::span<const std::uint8_t>{gp_entry->data}.subspan(4)};
                while (!reader.eof()) {
                    const auto f = reader.field();
                    if (!f) break;
                    if (f->number == 2 && f->wire_type == 2) {
                        proto::Reader row{reader.bytes().value_or(std::span<const std::uint8_t>{})};
                        std::string rk, rv;
                        while (!row.eof()) {
                            const auto rf = row.field();
                            if (!rf) break;
                            if (rf->wire_type == 2) {
                                const auto s = row.string().value_or("");
                                if (rf->number == 1) rk = s;
                                else if (rf->number == 2) rv = s;
                            } else if (!row.skip(rf->wire_type)) {
                                break;
                            }
                        }
                        if (rk == "FavorabilityDailyLimit" && !rv.empty()) {
                            daily_limit = std::atoi(rv.c_str());
                            limits_valid = true;
                        } else if (rk == "FavorabilityHeroDailyLimit" && !rv.empty()) {
                            hero_limit = std::atoi(rv.c_str());
                            limits_valid = true;
                        }
                    } else if (!reader.skip(f->wire_type)) {
                        break;
                    }
                }
            }
            if (limits_valid) {
                auto& p = account.player;
                const auto now = static_cast<std::int32_t>(std::time(nullptr));
                const auto day = static_cast<std::int32_t>((now + kServerUtcOffset) / 86400);
                if (p.touch_day != day) {
                    p.touch_day = day;
                    p.touch_total = 0;
                    p.touch_hero.clear();
                }
                if (p.touch_total >= daily_limit || p.touch_hero[target_hero] >= hero_limit) {
                    return {.ok = false, .code = 72, .opt = request.opt, // E_LIMIT_DAILY_FAVOR
                            .option_id = request.option_id, .hero_id = target_hero};
                }
                p.touch_total += 1;
                p.touch_hero[target_hero] += 1;
            }
        }
    } else { // opt == 2: 赠送礼物 (Send Gift)
        if (request_num <= 0) {
            return {.ok = false, .code = 1, .opt = request.opt, .option_id = request.option_id, .hero_id = request.hero_id};
        }
        auto item_it = std::ranges::find(account.items, request.option_id, &AccountItem::id);
        if (item_it == account.items.end() || item_it->num < request.num) {
            return {.ok = false, .code = 78, .opt = request.opt, .option_id = request.option_id, .hero_id = request.hero_id};
        }

        // 读取数据表判定是否喜爱礼物以及经验产出
        bool is_favorite = false;
        if (tables) {
            GameTable gift_ctrl;
            if (gift_ctrl.load(*tables, "SendGiftControl")) {
                if (const auto row = gift_ctrl.row(static_cast<std::uint64_t>(request.hero_id))) {
                    const auto favs = gift_ctrl.ints_field(*row, 2); // FavoriteGoodID
                    if (std::ranges::find(favs, request.option_id) != favs.end()) {
                        is_favorite = true;
                    }
                }
            }
            GameTable items;
            if (items.load(*tables, "Item")) {
                if (const auto row = items.row(static_cast<std::uint64_t>(request.option_id))) {
                    const auto eff = items.ints_field(*row, 22); // EffData
                    if (!eff.empty()) {
                        if (is_favorite && eff.size() > 1) {
                            exp_per_action = eff[1];
                        } else {
                            exp_per_action = eff[0];
                        }
                    }
                }
            }
        }

        // 扣除道具
        item_it->num -= request.num;
        new_item_count = item_it->num;
        if (item_it->num <= 0) {
            account.items.erase(item_it);
        }
        ++account.player.stat_gift_given; // 成就: 累计赠送礼物 (620290)
    }

    auto hero_it = std::ranges::find(account.heroes, target_hero, &AccountHero::id);
    if (hero_it == account.heroes.end()) {
        return {.ok = false, .code = 713, .opt = request.opt, .option_id = request.option_id, .hero_id = target_hero};
    }

    // 神格好感初始等级与等级上限
    std::int32_t initial_level = 1;
    std::int32_t level_limit = 20;
    if (tables) {
        GameTable hero_tab;
        if (hero_tab.load(*tables, "FavorabilityHero")) {
            if (const auto row = hero_tab.row(static_cast<std::uint64_t>(target_hero))) {
                initial_level = hero_tab.int_field(*row, 2).value_or(1);
                level_limit = hero_tab.int_field(*row, 3).value_or(20);
            }
        }
    }

    // 经验与好感等级
    const auto before_level = hero_it->favor_level > 0 ? hero_it->favor_level : initial_level;
    const auto before_exp = hero_it->favor_exp;
    auto cur_level = before_level;
    auto cur_exp = before_exp + exp_per_action * request_num;

    GameTable favor_levels;
    bool has_levels = tables && favor_levels.load(*tables, "FavorabilityLevel");

    while (cur_level < level_limit) {
        std::int32_t is_break = 0;
        if (has_levels) {
            if (const auto row = favor_levels.row(static_cast<std::uint64_t>(cur_level))) {
                is_break = favor_levels.int_field(*row, 3).value_or(0); // IsBreak
            }
        }

        std::int32_t req_exp = 0;
        if (has_levels) {
            if (const auto next_row = favor_levels.row(static_cast<std::uint64_t>(cur_level + 1))) {
                req_exp = favor_levels.int_field(*next_row, 2).value_or(0); // Exp
            }
        } else {
            req_exp = 100 * cur_level;
        }

        if (req_exp <= 0) break;

        if (cur_exp >= req_exp) {
            if (is_break > 0) {
                cur_exp = req_exp;
                break;
            }
            cur_exp -= req_exp;
            cur_level += 1;
        } else {
            break;
        }
    }

    if (cur_level >= level_limit) {
        cur_level = level_limit;
        cur_exp = 0;
    }

    hero_it->favor_level = cur_level;
    hero_it->favor_exp = cur_exp;

    // 触摸互动，每日任务【与神格互动】(TaskCondition = 19)
    if (request.opt == 1 && tables) {
        count_daily_task(account, *tables, 19, target_hero, 1);
        // 解锁一条触摸互动语音 (1350101: 点击解锁)
        GameTable dubbings;
        if (dubbings.load(*tables, "Dubbing")) {
            for (const auto& row : dubbings.rows()) {
                if (dubbings.int_field(row, 2).value_or(0) == target_hero &&
                    dubbings.int_field(row, 7).value_or(0) == 0 &&
                    dubbings.int_field(row, 8).value_or(0) == 1350101) {
                    const auto dub_id = static_cast<std::int32_t>(row.key);
                    if (dub_id > 0 && !std::ranges::contains(hero_it->dubbings, dub_id)) {
                        hero_it->dubbings.push_back(dub_id);
                        break;
                    }
                }
            }
        }
    }

    const auto full_dubbings = dubbing_unlock_ids(*hero_it, tables);

    AddFavorResult res;
    res.ok = true;
    res.code = 10; // LogicCode::Ok
    res.opt = request.opt;
    res.option_id = request.option_id;
    res.hero_id = target_hero;
    res.before_level = before_level;
    res.before_exp = before_exp;
    res.after_level = cur_level;
    res.after_exp = cur_exp;
    res.gifts_times = request.num;
    res.item_id = request.option_id;
    res.new_item_count = new_item_count;
    res.updated_hero = *hero_it;
    res.unlocked_dubbings = full_dubbings;
    return res;
}

UpgradeFettersResult apply_upgrade_fetters(Account& account, const TableBlob* tables,
                                           const UpgradeFettersRequest& request) {
    if (request.hero_id <= 0 || request.position_id <= 0) {
        return {.ok = false, .code = 13};
    }

    auto hero_it = std::find_if(account.heroes.begin(), account.heroes.end(),
                                [id = request.hero_id](const auto& h) { return h.id == id; });
    if (hero_it == account.heroes.end()) {
        return {.ok = false, .code = 13};
    }

    GameTable fetters_tab;
    const TableRow* matched_row = nullptr;
    std::optional<TableRow> row_storage;
    if (tables && fetters_tab.load(*tables, "FavorabilityFetters")) {
        const std::uint64_t key = (static_cast<std::uint64_t>(request.position_id) << 32) |
                                  static_cast<std::uint64_t>(request.hero_id);
        row_storage = fetters_tab.row(key);
        if (row_storage) {
            matched_row = &(*row_storage);
        } else {
            for (const auto& r : fetters_tab.rows()) {
                if (fetters_tab.int_field(r, 1).value_or(0) == request.hero_id &&
                    fetters_tab.int_field(r, 2).value_or(0) == request.position_id) {
                    matched_row = &r;
                    break;
                }
            }
        }
    }

    if (!matched_row) {
        return {.ok = false, .code = 13};
    }

    const auto is_open = fetters_tab.int_field(*matched_row, 12).value_or(1);
    if (is_open == 0) {
        return {.ok = false, .code = 13};
    }

    const auto levels = fetters_tab.ints_field(*matched_row, 4);
    const auto curr_id = fetters_tab.int_field(*matched_row, 8).value_or(901);
    const auto curr_nums = fetters_tab.ints_field(*matched_row, 9);

    const auto cur_lv = hero_it->fetters.contains(request.position_id)
                            ? hero_it->fetters[request.position_id]
                            : 0;

    int cur_idx = -1;
    for (int i = 0; i < static_cast<int>(levels.size()); ++i) {
        if (levels[i] == cur_lv) {
            cur_idx = i;
            break;
        }
    }

    const int target_idx = cur_idx + 1;
    if (target_idx < 0 || target_idx >= static_cast<int>(levels.size())) {
        return {.ok = false, .code = 10, .hero_id = request.hero_id,
                .position_id = request.position_id, .new_level = cur_lv,
                .updated_hero = *hero_it};
    }

    const auto next_lv = levels[target_idx];
    const auto cost = (target_idx < static_cast<int>(curr_nums.size())) ? curr_nums[target_idx] : 10000;

    if (curr_id == 901) {
        if (account.player.gold < cost) {
            return {.ok = false, .code = 202}; // E_LackMoney
        }
        account.player.gold -= cost;
    }

    hero_it->fetters[request.position_id] = next_lv;

    UpgradeFettersResult res;
    res.ok = true;
    res.code = 10;
    res.hero_id = request.hero_id;
    res.position_id = request.position_id;
    res.new_level = next_lv;
    res.updated_hero = *hero_it;
    res.gold_changed = (curr_id == 901);
    return res;
}

FavorBreakResult apply_favor_break(Account& account, const TableBlob* tables,
                                   const FavorBreakRequest& request) {
    if (request.hero_id <= 0) {
        return {.ok = false, .code = 13};
    }

    auto hero_it = std::find_if(account.heroes.begin(), account.heroes.end(),
                                [id = request.hero_id](const auto& h) { return h.id == id; });
    if (hero_it == account.heroes.end()) {
        return {.ok = false, .code = 13};
    }

    const auto cur_level = hero_it->favor_level > 0 ? hero_it->favor_level : 1;

    GameTable favor_levels;
    if (!tables || !favor_levels.load(*tables, "FavorabilityLevel")) {
        return {.ok = false, .code = 13};
    }

    const auto cur_row = favor_levels.row(static_cast<std::uint64_t>(cur_level));
    if (!cur_row) {
        return {.ok = false, .code = 13};
    }

    const auto is_break = favor_levels.int_field(*cur_row, 3).value_or(0);
    if (is_break <= 0) {
        return {.ok = false, .code = 13};
    }

    const auto break_items = favor_levels.ints_field(*cur_row, 4);
    const auto break_nums = favor_levels.ints_field(*cur_row, 5);

    for (std::size_t i = 0; i < break_items.size(); ++i) {
        const auto item_id = break_items[i];
        const auto req_num = (i < break_nums.size()) ? break_nums[i] : 1;
        const auto item_it = std::ranges::find(account.items, item_id, &AccountItem::id);
        if (item_it == account.items.end() || item_it->num < req_num) {
            return {.ok = false, .code = 201};
        }
    }

    std::vector<AccountItem> changed_items;
    for (std::size_t i = 0; i < break_items.size(); ++i) {
        const auto item_id = break_items[i];
        const auto req_num = (i < break_nums.size()) ? break_nums[i] : 1;
        take_item(account, item_id, req_num, changed_items);
    }

    hero_it->favor_level = cur_level + 1;
    hero_it->favor_exp = 0;
    const auto full_dubbings = dubbing_unlock_ids(*hero_it, tables);
    hero_it->dubbings = full_dubbings;

    FavorBreakResult res;
    res.ok = true;
    res.code = 10;
    res.hero_id = request.hero_id;
    res.before_level = cur_level;
    res.after_level = cur_level + 1;
    res.updated_hero = *hero_it;
    res.changed_items = std::move(changed_items);
    res.unlocked_dubbings = full_dubbings;
    return res;
}

// Dubbing.UnlockConditions holds a Language id (1350101..1350124, fixed table
// data). Classification mirrors the shipped texts: tap/gift voices are
// client-driven (the hero talking sends C2L_SaveHeroDubbing), breakthrough and
// favor-level voices derive from hero favor state, event voices (contract /
// login streaks / holidays) have no trigger on an offline server so they are
// granted outright.
namespace {
enum class DubbingCond : std::uint8_t { ClientDriven, BreakCount, FavorLevel, Auto };
struct DubbingCondRule {
    std::int32_t lang_id;
    DubbingCond kind;
    std::int32_t param;
};
constexpr DubbingCondRule kDubbingCondRules[] = {
    {1350101, DubbingCond::ClientDriven, 0}, // [点击解锁]
    {1350102, DubbingCond::BreakCount, 1},   // [默契度第1次突破后解锁]
    {1350103, DubbingCond::BreakCount, 2},   // [默契度第2次突破后解锁]
    {1350104, DubbingCond::BreakCount, 3},   // [默契度第3次突破后解锁]
    {1350105, DubbingCond::Auto, 0},         // [契约后解锁]
    {1350106, DubbingCond::Auto, 0},         // [连续登录3天点击解锁]
    {1350107, DubbingCond::Auto, 0},         // [连续登录7天点击解锁]
    {1350108, DubbingCond::Auto, 0},         // [连续登录30天点击解锁]
    {1350109, DubbingCond::Auto, 0},         // [累计登录50天点击解锁]
    {1350110, DubbingCond::Auto, 0},         // [累计登录100天点击解锁]
    {1350111, DubbingCond::ClientDriven, 0}, // [收到喜欢的礼物解锁]
    {1350112, DubbingCond::Auto, 0},         // [元旦解锁]
    {1350113, DubbingCond::Auto, 0},         // [春节解锁]
    {1350114, DubbingCond::Auto, 0},         // [情人节解锁]
    {1350115, DubbingCond::Auto, 0},         // [愚人节解锁]
    {1350116, DubbingCond::Auto, 0},         // [儿童节解锁]
    {1350117, DubbingCond::Auto, 0},         // [夏日祭解锁]
    {1350118, DubbingCond::Auto, 0},         // [返校季解锁]
    {1350119, DubbingCond::Auto, 0},         // [秋日祭解锁]
    {1350120, DubbingCond::Auto, 0},         // [冬日祭解锁]
    {1350121, DubbingCond::Auto, 0},         // [神格生日解锁]
    {1350122, DubbingCond::Auto, 0},         // [解神者生日解锁]
    {1350123, DubbingCond::Auto, 0},         // [默认解锁]
    {1350124, DubbingCond::FavorLevel, 5},   // [默契度5级后点击解锁]
};
} // namespace

std::vector<std::int32_t> dubbing_unlock_ids(const AccountHero& hero, const TableBlob* tables) {
    std::vector<std::int32_t> ids = hero.dubbings;
    const auto add_id = [&ids](std::int32_t id) {
        if (id > 0 && !std::ranges::contains(ids, id)) ids.push_back(id);
    };
    if (!tables) return ids;

    GameTable dubbings;
    GameTable favor_levels;
    if (!dubbings.load(*tables, "Dubbing") || !favor_levels.load(*tables, "FavorabilityLevel")) return ids;

    std::int32_t break_count = 0; // breakthrough rows sit at levels 4/9/14/19
    for (const auto& row : favor_levels.rows()) {
        const auto level = static_cast<std::int32_t>(row.key);
        if (favor_levels.int_field(row, 3).value_or(0) > 0 && hero.favor_level > level) ++break_count;
    }

    for (const auto& row : dubbings.rows()) {
        if (dubbings.int_field(row, 2).value_or(0) != hero.id) continue;     // HeroId
        if (dubbings.int_field(row, 7).value_or(0) != 0) continue;           // Locked: client-free
        const auto* rule = std::ranges::find(kDubbingCondRules, dubbings.int_field(row, 8).value_or(0),
                                             &DubbingCondRule::lang_id);
        if (rule == std::end(kDubbingCondRules)) continue;
        bool unlocked = false;
        switch (rule->kind) {
            case DubbingCond::Auto: unlocked = true; break;
            case DubbingCond::BreakCount: unlocked = break_count >= rule->param; break;
            case DubbingCond::FavorLevel: unlocked = hero.favor_level >= rule->param; break;
            case DubbingCond::ClientDriven: unlocked = false; break; // hero talk -> C2L_SaveHeroDubbing
        }
        if (unlocked) add_id(static_cast<std::int32_t>(row.key));
    }
    return ids;
}


// ---- 白夜行星 (CollegeModule) building economy ----

namespace {

bool pay_college_costs(Account& account, const TableBlob* tables, const GameTable& table,
                       const TableRow& row, std::vector<AccountItem>& changed, bool& wallet_changed,
                       std::int32_t& err_code) {
    const auto items = table.ints_field(row, 9);  // f9 item ids
    const auto nums = table.ints_field(row, 10);  // f10 counts (parallel)
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto id = items[i];
        const auto num = i < nums.size() ? nums[i] : 0;
        if (id <= 0 || num <= 0) continue;
        if (const auto enu = item_currency_enu(tables, id)) {
            auto* purse = wallet(account.player, *enu);
            if (!purse || *purse < num) {
                err_code = kCollegeLimitGold;
                return false;
            }
            *purse -= num;
            wallet_changed = true;
        } else if (!take_item(account, id, num, changed)) {
            err_code = kCollegeNoItem;
            return false;
        }
    }
    return true;
}

} // namespace

std::pair<std::int32_t, std::int32_t> college_state(const Account& account, const TableBlob* tables,
                                                    std::int32_t building_id) {
    if (const auto it = account.player.college.find(building_id); it != account.player.college.end()) {
        return it->second;
    }
    std::int32_t level = 1, star = 0;
    if (tables) {
        GameTable buildings;
        if (buildings.load(*tables, "CollegeBuilding")) {
            if (const auto row = buildings.row(static_cast<std::uint64_t>(building_id))) {
                level = buildings.int_field(*row, 10).value_or(1); // f10 InitialLevel
                star = buildings.int_field(*row, 11).value_or(0);  // f11 InitialStar
            }
        }
    }
    return {level, star};
}

std::int32_t finish_expired_college_build(Account& account, std::int32_t now_unix) {
    auto& q = account.player.build_queue;
    if (q.building_id <= 0 || now_unix < q.end_unix) return 0;
    auto& [level, star] = account.player.college[q.building_id];
    level += 1;
    const auto done = q.building_id;
    q = {};
    return done;
}

CollegeUpgradeResult start_college_upgrade(Account& account, const TableBlob* tables,
                                            std::int32_t building_id, std::int32_t now_unix) {
    CollegeUpgradeResult res;
    res.building_id = building_id;
    if (!tables || building_id <= 0) {
        res.code = 13; // E_ERROR_OPT
        return res;
    }
    if (account.player.build_queue.building_id > 0) {
        res.code = kCollegeBusy;
        return res;
    }
    GameTable buildings, levels;
    if (!buildings.load(*tables, "CollegeBuilding") || !levels.load(*tables, "CollegeLevel")) {
        res.code = 13;
        return res;
    }
    const auto brow = buildings.row(static_cast<std::uint64_t>(building_id));
    if (!brow || buildings.int_field(*brow, 2).value_or(1) != 1) { // type-1 college building
        res.code = 13;
        return res;
    }
    res.type = 1;
    const auto [level, star] = college_state(account, tables, building_id);
    const auto level_limit = buildings.int_field(*brow, 12).value_or(30); // f12 LevelLimited
    if (level >= level_limit) {
        res.code = kCollegeMaxLevel;
        return res;
    }
    // CollegeLevel row for the level being built: f3 buildingId + f4 level.
    const TableRow* lrow = nullptr;
    for (const auto& row : levels.rows()) {
        if (levels.int_field(row, 3).value_or(0) == building_id &&
            levels.int_field(row, 4).value_or(0) == level + 1) {
            lrow = &row;
            break;
        }
    }
    if (!lrow) {
        res.code = 29; // E_ERR_CONFIG
        return res;
    }
    if (!pay_college_costs(account, tables, levels, *lrow, res.changed_items, res.wallet_changed, res.code)) {
        return res;
    }
    res.duration_sec = levels.int_field(*lrow, 8).value_or(0); // f8 build seconds
    res.level = level + 1;
    res.end_unix = static_cast<std::int64_t>(now_unix) + res.duration_sec;
    account.player.college[building_id] = {level, star};
    account.player.build_queue = {building_id, res.end_unix, res.duration_sec};
    res.ok = true;
    return res;
}

CollegeSpeedResult speed_up_college_build(Account& account, const TableBlob* tables,
                                           std::int32_t building_id, std::int32_t item_id,
                                           std::int32_t item_num, std::int32_t now_unix) {
    CollegeSpeedResult res;
    res.building_id = building_id;
    res.item_id = item_id;
    res.item_num = item_num;
    if (building_id <= 0 || account.player.build_queue.building_id != building_id) {
        res.code = 13;
        return res;
    }
    if (item_id > 0 && item_num > 0) {
        if (const auto enu = item_currency_enu(tables, item_id)) {
            auto* purse = wallet(account.player, *enu);
            if (!purse || *purse < item_num) {
                res.code = kCollegeLimitGold;
                return res;
            }
            *purse -= item_num;
            res.wallet_changed = true;
        } else if (!take_item(account, item_id, item_num, res.changed_items)) {
            res.code = kCollegeNoItem;
            return res;
        }
    }
    account.player.build_queue.end_unix = now_unix; // finish now
    res.type = 1;
    res.ok = finish_expired_college_build(account, now_unix) == building_id;
    if (!res.ok) res.code = 13;
    return res;
}

CollegeStarResult star_up_college(Account& account, const TableBlob* tables, std::int32_t building_id) {
    CollegeStarResult res;
    res.building_id = building_id;
    if (!tables || building_id <= 0) {
        res.code = 13;
        return res;
    }
    GameTable buildings, stars;
    if (!buildings.load(*tables, "CollegeBuilding") || !stars.load(*tables, "CollegeStarLevel")) {
        res.code = 13;
        return res;
    }
    const auto brow = buildings.row(static_cast<std::uint64_t>(building_id));
    if (!brow) {
        res.code = 13;
        return res;
    }
    const auto is_wonder = buildings.int_field(*brow, 2).value_or(1) == 2;
    res.type = is_wonder ? 2 : 1;
    const auto star_limit = buildings.int_field(*brow, 13).value_or(6); // f13 StarLimited
    const auto [level, star] = college_state(account, tables, building_id);
    if (star >= star_limit) {
        res.code = kCollegeMaxLevel;
        return res;
    }
    // Star row keyed by CollegeBuilding.StarID[star] (f15 list, index = star).
    const auto star_ids = buildings.ints_field(*brow, 15);
    if (star < static_cast<std::int32_t>(star_ids.size())) {
        if (const auto srow = stars.row(static_cast<std::uint64_t>(star_ids[star]))) {
            if (!pay_college_costs(account, tables, stars, *srow, res.changed_items,
                                   res.wallet_changed, res.code)) {
                return res; // star-1 rows are free (no f9/f10)
            }
        }
    }
    account.player.college[building_id] = {level, star + 1};
    res.star = star + 1;
    res.ok = true;
    return res;
}

} // namespace x2::offline
