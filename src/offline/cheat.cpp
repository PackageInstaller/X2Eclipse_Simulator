#include "offline/cheat.hpp"

#include <algorithm>
#include <cstdlib>
#include <random>
#include <ctime>
#include <limits>
#include <ranges>
#include <span>

#include "offline/game_table.hpp"
#include "offline/mission.hpp"
#include "proto/protobuf.hpp"
#include "server/websocket.hpp"

namespace x2::offline {

namespace {

// CheatOpt values (il2cpp.cs, enum CheatOpt).
enum Opt : std::int32_t {
    CO_MOBILITY = 0,
    CO_GOLD = 1,
    CO_DIAMOND = 2,
    CO_EQUIP_EXP = 3,
    CO_UNLOCK_HERO = 4,
    CO_TRY_HERO = 5,
    CO_HERO_UPLEVEL = 6,
    CO_HERO_UPSTAR = 7,
    CO_ITEM = 8,
    CO_INIT_ACCOUNT = 13,
    CO_PLAYER_LEVEL = 16,
    CO_PLAYER_EXP = 17,
    CO_NormalDust = 18,
    CO_MAIL_ITEM = 19,
    CO_HERO_FAVOR_EXP = 22,
    CO_HERO_FAVOR_LEVEL = 23,
    CO_HERO_FAVOR_CLEAR = 24,
};

std::int64_t value_at(const CheatRequest& request, std::size_t index) {
    return index < request.values.size() ? request.values[index] : 0;
}

std::int32_t clamp_i32(std::int64_t value) {
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(value, 0, std::numeric_limits<std::int32_t>::max()));
}

void update(std::int32_t& field, const CheatRequest& request) {
    const std::int64_t value = value_at(request, 0);
    field = clamp_i32(request.isset ? value : field + value);
}

AccountHero& hero(Account& account, const TableBlob* tables, std::int32_t id) {
    for (auto& h : account.heroes) {
        if (h.id == id) return h;
    }
    return account.heroes.emplace_back(
        AccountHero{.id = id, .state = 2, .level = 1, .star = default_hero_star(tables, id)});
}

CheatEffect hero_op(Account& account, const TableBlob* tables, const CheatRequest& request,
                    std::int32_t AccountHero::*field) {
    const auto id = static_cast<std::int32_t>(value_at(request, 0));
    if (id <= 0) return {.supported = false};
    auto& h = hero(account, tables, id);
    if (field != nullptr) {
        auto next = std::max(1, clamp_i32(h.*field + value_at(request, 1)));
        if (field == &AccountHero::level && account.player.level >= 1) {
            next = h.level >= account.player.level ? h.level : std::min(next, account.player.level);
        }
        if (field == &AccountHero::star) next = std::min(next, max_hero_star(tables));
        h.*field = next;
    }
    return {.player = true, .heroes = {h}};
}

std::int32_t favor_field(const TableBlob* tables, const char* table, std::int32_t hero_id, std::uint32_t field,
                         std::int32_t fallback) {
    if (!tables) return fallback;
    GameTable rows;
    if (!rows.load(*tables, table)) return fallback;
    const auto row = rows.row(static_cast<std::uint64_t>(hero_id));
    const auto value = row ? rows.int_field(*row, field).value_or(fallback) : fallback;
    return value > 0 ? value : fallback;
}

std::int32_t favor_bar(const TableBlob* tables, std::int32_t level) {
    if (!tables || level <= 0) return 0;
    GameTable rows;
    if (!rows.load(*tables, "FavorabilityLevel")) return 0;
    const auto row = rows.row(static_cast<std::uint64_t>(level + 1));
    return row ? rows.int_field(*row, 2).value_or(0) : 0;
}

CheatEffect favor_op(Account& account, const TableBlob* tables, const CheatRequest& request, Opt opt) {
    const auto id = static_cast<std::int32_t>(value_at(request, 0));
    if (id <= 0) return {.supported = false};
    if (std::ranges::find(account.heroes, id, &AccountHero::id) == account.heroes.end()) return {.supported = false};
    auto& h = hero(account, tables, id);
    const auto initial = favor_field(tables, "FavorabilityHero", id, 2, 1);
    const auto limit = favor_field(tables, "FavorabilityHero", id, 3, 20);
    const auto before_level = h.favor_level > 0 ? h.favor_level : initial;
    const auto before_exp = h.favor_exp;
    auto level = before_level;
    auto exp = before_exp;
    if (opt == CO_HERO_FAVOR_CLEAR) {
        level = initial;
        exp = 0;
    } else if (opt == CO_HERO_FAVOR_LEVEL) {
        auto delta = value_at(request, 1);
        if (delta <= 0) delta = 1;
        level = std::min(limit, clamp_i32(level + delta));
    } else {
        auto delta = value_at(request, 1);
        if (delta <= 0) delta = 1;
        exp = clamp_i32(exp + delta);
        if (const auto bar = favor_bar(tables, level); bar > 0) exp = std::min(exp, bar);
    }
    h.favor_level = level;
    h.favor_exp = exp;
    return {.player = true,
            .favor_hero = id,
            .favor_before_level = before_level,
            .favor_before_exp = before_exp,
            .favor_after_level = level,
            .favor_after_exp = exp};
}

} // namespace

CheatRequest parse_cheat(std::span<const std::uint8_t> body) {
    CheatRequest request;
    proto::Reader reader{body};
    while (!reader.eof()) {
        const auto field = reader.field();
        if (!field) break;
        if (field->number == 1 && field->wire_type == 0) {
            request.opt = static_cast<std::int32_t>(reader.varint().value_or(0));
        } else if (field->number == 2 && field->wire_type == 0) {
            request.isset = reader.varint().value_or(0) != 0;
        } else if (field->number == 3 && field->wire_type == 0) {
            request.values.push_back(static_cast<std::int64_t>(reader.varint().value_or(0)));
        } else if (field->number == 4 && field->wire_type == 2) {
            request.strvals.push_back(reader.string().value_or(""));
        } else if (!reader.skip(field->wire_type)) {
            break;
        }
    }
    return request;
}

CheatEffect apply_cheat(Account& account, const CheatRequest& request, const TableBlob* tables) {
    auto& p = account.player;
    std::int32_t level_before = 0, level_after = 0;
    switch (request.opt) {
        case CO_MOBILITY: update(p.power, request); break;
        case CO_GOLD: update(p.gold, request); break;
        case CO_DIAMOND: update(p.crystal, request); break;
        case CO_EQUIP_EXP: update(p.equip_exp, request); break;
        case CO_NormalDust: update(p.jewel_chip, request); break;
        case CO_PLAYER_LEVEL: {
            level_before = p.level;
            update(p.level, request);
            p.level = std::clamp(p.level, 1, max_role_level(tables));
            const auto gifted_through = std::max(level_before, p.rewarded_level);
            grant_role_level_gifts(account, tables, std::min(gifted_through, p.level), p.level);
            p.rewarded_level = std::max(p.rewarded_level, p.level);
            if (tables && p.level > level_before) seed_guides(account, *tables);
            level_after = p.level;
            break;
        }
        case CO_PLAYER_EXP: update(p.exp, request); break;
        case CO_INIT_ACCOUNT: p = AccountPlayer{}; break;
        case CO_UNLOCK_HERO: return hero_op(account, tables, request, nullptr);
        case CO_TRY_HERO:
            return {};
        case CO_HERO_UPLEVEL: return hero_op(account, tables, request, &AccountHero::level);
        case CO_HERO_UPSTAR: return hero_op(account, tables, request, &AccountHero::star);
        case CO_MAIL_ITEM: {
            const auto id = static_cast<std::int32_t>(value_at(request, 0));
            auto num = static_cast<std::int32_t>(value_at(request, 1));
            if (id <= 0) return {.supported = false};
            if (num <= 0) num = 1;
            std::int64_t mail_id = 1;
            for (const auto& mail : account.player.mails) mail_id = std::max(mail_id, mail.id + 1);
            account.player.mails.push_back(AccountMail{
                .id = mail_id,
                .item_id = id,
                .num = num,
                .time = static_cast<std::int64_t>(std::time(nullptr)),
                .state = 0,
            });
            x2::server::ws::WebSocketManager::instance().broadcast_mail(mail_id);
            return {};
        }
        case CO_HERO_FAVOR_EXP:
        case CO_HERO_FAVOR_LEVEL:
        case CO_HERO_FAVOR_CLEAR: return favor_op(account, tables, request, static_cast<Opt>(request.opt));
        case 15: { // CO_HEROEQUIP_MAKE_LUCK: values[0]=EquibBase id 随机词条打造一件
            const auto type_id = static_cast<std::int32_t>(value_at(request, 0));
            if (!tables || type_id <= 0) return {.supported = false};
            GameTable base, attrib;
            if (!base.load(*tables, "EquibBase") || !attrib.load(*tables, "EquibAttrib")) {
                return {.supported = false};
            }
            const auto brow = base.row(static_cast<std::uint64_t>(type_id));
            if (!brow) return {.supported = false};
            constexpr std::int32_t kRare = 6; // GMAdvanceAccount 高级账号
            // 主属性: MainAttrType[0] (f4), 值 = EquibAttrib(rare, SRC=0, type) ValueSec 随机
            const auto main_types = base.ints_field(*brow, 4);
            if (main_types.empty()) return {.supported = false};
            auto pick_value = [&](std::int32_t attr_type, std::int32_t src) -> std::int32_t {
                for (const auto& arow : attrib.rows()) {
                    if (attrib.int_field(arow, 1).value_or(0) != kRare) continue;
                    if (attrib.int_field(arow, 2).value_or(0) != src) continue;
                    if (attrib.int_field(arow, 3).value_or(0) != attr_type) continue;
                    const auto sec = attrib.ints_field(arow, 4);
                    if (sec.empty()) return 0;
                    return sec[std::rand() % static_cast<std::int32_t>(sec.size())];
                }
                return 0;
            };
            AccountEquip equip;
            static std::mt19937 rng{std::random_device{}()};
            std::int32_t max_id = 20000000;
            for (const auto& e : account.player.equips) max_id = std::max(max_id, e.id);
            equip.id = max_id + 1;
            equip.type_id = type_id;
            equip.star = kRare;
            equip.attrs[main_types[0]] = pick_value(main_types[0], 0);
            // 小词条: 数量按 EquibStage[star].MinorListMin/Max (表: star1→1 ... star5-6→4/5)
            std::int32_t minor_min = 4, minor_max = 5;
            if (GameTable stage_tbl; stage_tbl.load(*tables, "EquibStage")) {
                if (const auto s_row = stage_tbl.row(static_cast<std::uint64_t>(kRare))) {
                    minor_min = stage_tbl.int_field(*s_row, 2).value_or(4); // f2 MinorListMin
                    minor_max = stage_tbl.int_field(*s_row, 3).value_or(5); // f3 MinorListMax
                }
            }
            const auto minor_count = minor_min + (minor_max > minor_min ? std::rand() % (minor_max - minor_min + 1) : 0);
            // MinorAttrType (f6) 按 MinorAttrChance (f7) 权重抽不重复
            const auto minor_types = base.ints_field(*brow, 6);
            const auto minor_chance = base.ints_field(*brow, 7);
            std::int32_t total = 0;
            for (const auto c : minor_chance) total += c;
            for (std::int32_t picked = 0; picked < minor_count && !minor_types.empty() && total > 0; ++picked) {
                for (std::int32_t attempt = 0; attempt < 32; ++attempt) {
                    std::int32_t roll = std::rand() % total;
                    std::size_t idx = 0;
                    for (; idx < minor_chance.size(); ++idx) {
                        roll -= minor_chance[idx];
                        if (roll < 0) break;
                    }
                    if (idx >= minor_types.size()) idx = minor_types.size() - 1;
                    const auto t = minor_types[idx];
                    if (t <= 0 || equip.attrs.contains(t)) continue;
                    equip.attrs[t] = pick_value(t, 1);
                    break;
                }
            }
            account.player.equips.push_back(equip);
            CheatEffect result;
            result.player = true; // PlayerData 含 equip_all 全量
            return result;
        }
        case 50: { // CO_MULTI_ITEM_GET: values=[itemId...], strvals[0]=数量 (GMAdvanceAccount Add 行)
            if (request.values.empty() || request.strvals.empty()) return {.supported = false};
            const auto num = clamp_i32(std::strtoll(request.strvals[0].c_str(), nullptr, 10));
            if (num <= 0) return {.supported = false};
            CheatEffect result;
            for (const auto v : request.values) {
                const auto id = static_cast<std::int32_t>(v);
                if (id <= 0) continue;
                if (const auto enu = item_currency_enu(tables, id)) {
                    if (auto* field = wallet(account.player, *enu)) *field = clamp_i32(*field + num);
                    result.player = true;
                    continue;
                }
                auto it = std::ranges::find(account.items, id, &AccountItem::id);
                if (it == account.items.end()) it = account.items.insert(it, AccountItem{.id = id});
                it->num = clamp_i32(it->num + num);
                result.items.push_back(*it);
            }
            return result;
        }
        case 29: { // CO_UNLOCK_ALL_SCECTION: values[0]=目标 section, 解锁其之前的全部主线关
            const auto target = static_cast<std::int32_t>(value_at(request, 0));
            if (!tables || target <= 0) return {.supported = false};
            GameTable section;
            if (!section.load(*tables, "SectionTable")) return {.supported = false};
            for (const auto& row : section.rows()) {
                // Type (f8) E_guanqia=0 = 主线; 只补 target 及之前的
                if (section.int_field(row, 8).value_or(-1) != 0) continue;
                const auto key = static_cast<std::int32_t>(row.key);
                if (key > target) continue;
                if (!std::ranges::contains(account.player.cleared_main, key)) {
                    account.player.cleared_main.push_back(key);
                }
            }
            account.player.main_chapter = account.player.main_chapter > 0 ? account.player.main_chapter : 0;
            return {.player = true};
        }
        case CO_ITEM: {
            // values: [itemId, num]
            const auto id = static_cast<std::int32_t>(value_at(request, 0));
            if (id <= 0) return {.supported = false};
            const auto num = value_at(request, 1);
            if (const auto enu = item_currency_enu(tables, id)) {
                if (auto* field = wallet(account.player, *enu)) {
                    *field = clamp_i32(request.isset ? num : *field + num);
                }
                return {.player = true};
            }
            auto it = std::ranges::find(account.items, id, &AccountItem::id);
            if (it == account.items.end()) it = account.items.insert(it, AccountItem{.id = id});
            it->num = clamp_i32(request.isset ? num : it->num + num);
            return {.items = {*it}};
        }
        default: return {.supported = false};
    }
    return {.player = true, .level_before = level_before, .level_after = level_after};
}

int mark_mails_read(Account& account, std::span<const std::int64_t> ids) {
    int changed = 0;
    for (const auto id : ids) {
        const auto mail = std::ranges::find(account.player.mails, id, &AccountMail::id);
        if (mail == account.player.mails.end() || mail->state == 4) continue;
        if (mail->state == 0) mail->state = 1;
        else if (mail->state == 2) mail->state = 3;
        else continue;
        ++changed;
    }
    return changed;
}

int delete_mails(Account& account, std::span<const std::int64_t> ids) {
    int removed = 0;
    for (const auto id : ids) {
        const auto mail = std::ranges::find(account.player.mails, id, &AccountMail::id);
        if (mail == account.player.mails.end() || mail->state == 4) continue;
        mail->state = 4;
        ++removed;
    }
    return removed;
}

MailClaim claim_mail(Account& account, std::int64_t mail_id, const TableBlob* tables) {
    const auto mail = std::ranges::find(account.player.mails, mail_id, &AccountMail::id);
    if (mail == account.player.mails.end() || mail->state >= 2) return {};
    mail->state = 2; // MailState.Received

    auto paid = pay_rewards(account, tables, {{.id = mail->item_id, .num = mail->num}});
    MailClaim claim{
        .ok = true,
        .player = paid.player,
        .granted = mail->num,
        .bag_items = std::move(paid.bag),
        .equips = std::move(paid.equips),
        .shown = std::move(paid.shown),
    };
    if (!claim.bag_items.empty()) claim.bag = claim.bag_items[0];
    return claim;
}


} // namespace x2::offline
