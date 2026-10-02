#include "offline/account_repository.hpp"

#include "offline/game_table.hpp"
#include "offline/mission.hpp"
#include "offline/protocol_builders.hpp"
#include "persist/account_db.hpp"
#include "proto/protobuf.hpp"

#include <algorithm>
#include <map>
#include <ranges>
#include <utility>

namespace x2::offline {

namespace {

constexpr std::int32_t kDefaultHeroLevel = 1;
constexpr std::int32_t kDefaultHeroStar = 1;


void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 24));
}

bool read_u32(std::span<const std::uint8_t> in, std::size_t& pos, std::uint32_t& value) {
    if (pos + 4 > in.size()) return false;
    value = static_cast<std::uint32_t>(in[pos]) | (static_cast<std::uint32_t>(in[pos + 1]) << 8) |
            (static_cast<std::uint32_t>(in[pos + 2]) << 16) | (static_cast<std::uint32_t>(in[pos + 3]) << 24);
    pos += 4;
    return true;
}

std::vector<std::uint8_t> encode_heroes(const std::vector<AccountHero>& heroes) {
    std::vector<std::uint8_t> out;
    for (const auto& hero : heroes) {
        auto msg = ProtocolBuilders::hero_data(hero);
        if (hero.god_shed || !hero.god_slots.empty() || hero.favor_level > 0 || hero.favor_exp > 0 ||
            hero.artifact.id > 0 || hero.artifact.level > 0 || hero.artifact.star > 0 || !hero.artifact.jewels.empty() ||
            !hero.skills.empty() || !hero.archives.empty() || !hero.fetters.empty() || !hero.dubbings.empty()) {
            proto::Writer flag;
            if (hero.god_shed) flag.int32(30, 1);
            for (const auto slot : hero.god_slots) flag.int32(31, slot);
            if (hero.favor_level > 0) flag.int32(32, hero.favor_level);
            if (hero.favor_exp > 0) flag.int32(33, hero.favor_exp);
            if (hero.artifact.id > 0) flag.int32(34, hero.artifact.id);
            if (hero.artifact.level > 0) flag.int32(35, hero.artifact.level);
            if (hero.artifact.star > 0) flag.int32(36, hero.artifact.star);
            for (const auto& [slot, prop] : hero.artifact.properties) {
                proto::Writer prop_entry;
                prop_entry.int32(1, slot);
                prop_entry.int32(2, prop.first);
                prop_entry.int32(3, prop.second);
                flag.message(45, prop_entry.data());
            }
            for (const auto& [slot, j_id] : hero.artifact.jewels) {
                if (j_id > 0) {
                    proto::Writer pair;
                    pair.int32(1, slot);
                    pair.int32(2, j_id);
                    flag.message(37, pair.data());
                }
            }
            for (const auto& [s_id, s_lvl] : hero.skills) {
                if (s_id > 0 && s_lvl > 0) {
                    proto::Writer pair;
                    pair.int32(1, s_id);
                    pair.int32(2, s_lvl);
                    flag.message(38, pair.data());
                }
            }
            for (const auto& [f_id, f_status] : hero.archives) {
                if (f_id > 0 && f_status > 0) {
                    proto::Writer pair;
                    pair.int32(1, f_id);
                    pair.int32(2, f_status);
                    flag.message(39, pair.data());
                }
            }
            for (const auto& [pos_id, f_lv] : hero.fetters) {
                if (pos_id > 0 && f_lv > 0) {
                    proto::Writer pair;
                    pair.int32(1, pos_id);
                    pair.int32(2, f_lv);
                    flag.message(40, pair.data());
                }
            }
            for (const auto dub_id : hero.dubbings) {
                if (dub_id > 0) flag.int32(41, dub_id);
            }
            if (hero.battle_skin > 0) flag.int32(42, hero.battle_skin);
            if (hero.outer_skin > 0) flag.int32(43, hero.outer_skin);
            msg.insert(msg.end(), flag.data().begin(), flag.data().end());
        }
        append_u32(out, static_cast<std::uint32_t>(msg.size()));
        out.insert(out.end(), msg.begin(), msg.end());
    }
    return out;
}

std::vector<std::uint8_t> encode_items(const std::vector<AccountItem>& items) {
    std::vector<std::uint8_t> out;
    for (const auto& item : items) {
        const auto msg = ProtocolBuilders::item_data(item);
        append_u32(out, static_cast<std::uint32_t>(msg.size()));
        out.insert(out.end(), msg.begin(), msg.end());
    }
    return out;
}

std::vector<AccountHero> decode_heroes(std::span<const std::uint8_t> blob) {
    std::vector<AccountHero> out;
    std::size_t pos = 0;
    while (pos < blob.size()) {
        std::uint32_t size = 0;
        if (!read_u32(blob, pos, size) || pos + size > blob.size()) break;
        proto::Reader reader{{blob.data() + pos, size}};
        pos += size;
        AccountHero hero;
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            switch (field->number) {
                case 1: hero.id = static_cast<std::int32_t>(reader.varint().value_or(0)); break;
                case 2: hero.state = static_cast<std::int32_t>(reader.varint().value_or(0)); break;
                case 3: hero.level = static_cast<std::int32_t>(reader.varint().value_or(0)); break;
                case 4: hero.star = static_cast<std::int32_t>(reader.varint().value_or(0)); break;
                case 5: {
                    if (field->wire_type != 2) {
                        reader.skip(field->wire_type);
                        break;
                    }
                    proto::Reader god{reader.bytes().value_or(std::span<const std::uint8_t>{})};
                    while (!god.eof()) {
                        const auto gfield = god.field();
                        if (!gfield) break;
                        switch (gfield->number) {
                            case 1: if (hero.artifact.id == 0) hero.artifact.id = static_cast<std::int32_t>(god.varint().value_or(0)); break;
                            case 2: if (hero.artifact.level == 0) hero.artifact.level = static_cast<std::int32_t>(god.varint().value_or(0)); break;
                            case 3: if (hero.artifact.star == 0) hero.artifact.star = static_cast<std::int32_t>(god.varint().value_or(0)); break;
                            case 4: {
                                if (gfield->wire_type != 2) { god.skip(gfield->wire_type); break; }
                                proto::Reader jentry{god.bytes().value_or(std::span<const std::uint8_t>{})};
                                std::int32_t jslot = 0, jid = 0;
                                while (!jentry.eof()) {
                                    const auto jinner = jentry.field();
                                    if (!jinner || jinner->wire_type != 0) break;
                                    const auto val = static_cast<std::int32_t>(jentry.varint().value_or(0));
                                    if (jinner->number == 1) jslot = val;
                                    else if (jinner->number == 2) jid = val;
                                }
                                if (jid > 0 && !hero.artifact.jewels.contains(jslot)) hero.artifact.jewels[jslot] = jid;
                                break;
                            }
                            case 8: {
                                if (gfield->wire_type != 2) { god.skip(gfield->wire_type); break; }
                                proto::Reader entry{god.bytes().value_or(std::span<const std::uint8_t>{})};
                                std::int32_t slot = 0;
                                while (!entry.eof()) {
                                    const auto inner = entry.field();
                                    if (!inner || inner->wire_type != 0) break;
                                    const auto val = static_cast<std::int32_t>(entry.varint().value_or(0));
                                    if (inner->number == 1) slot = val;
                                }
                                if (slot > 0 && std::ranges::find(hero.god_slots, slot) == hero.god_slots.end())
                                    hero.god_slots.push_back(slot);
                                break;
                            }
                            default: god.skip(gfield->wire_type); break;
                        }
                    }
                    break;
                }
                case 6: {
                    if (field->wire_type != 2) {
                        reader.skip(field->wire_type);
                        break;
                    }
                    proto::Reader entry{reader.bytes().value_or(std::span<const std::uint8_t>{})};
                    std::int32_t slot = 0, equip_id = 0;
                    while (!entry.eof()) {
                        const auto inner = entry.field();
                        if (!inner || inner->wire_type != 0) break;
                        const auto value = static_cast<std::int32_t>(entry.varint().value_or(0));
                        if (inner->number == 1) slot = value;
                        else if (inner->number == 2) equip_id = value;
                    }
                    if (slot > 0 && equip_id > 0) hero.worn[slot] = equip_id;
                    break;
                }
                case 7: hero.exp = static_cast<std::int64_t>(reader.varint().value_or(0)); break;
                case 11: hero.name = reader.string().value_or(""); break;
                case 13: hero.get_time = static_cast<std::int64_t>(reader.varint().value_or(0)); break;
                case 30: hero.god_shed = reader.varint().value_or(0) != 0; break;
                case 31: hero.god_slots.push_back(static_cast<std::int32_t>(reader.varint().value_or(0))); break;
                case 32: hero.favor_level = static_cast<std::int32_t>(reader.varint().value_or(0)); break;
                case 33: hero.favor_exp = static_cast<std::int32_t>(reader.varint().value_or(0)); break;
                case 34: hero.artifact.id = static_cast<std::int32_t>(reader.varint().value_or(0)); break;
                case 35: hero.artifact.level = static_cast<std::int32_t>(reader.varint().value_or(0)); break;
                case 36: hero.artifact.star = static_cast<std::int32_t>(reader.varint().value_or(0)); break;
                case 37: {
                    if (field->wire_type != 2) {
                        reader.skip(field->wire_type);
                        break;
                    }
                    proto::Reader entry{reader.bytes().value_or(std::span<const std::uint8_t>{})};
                    std::int32_t slot = 0, j_id = 0;
                    while (!entry.eof()) {
                        const auto inner = entry.field();
                        if (!inner || inner->wire_type != 0) break;
                        const auto val = static_cast<std::int32_t>(entry.varint().value_or(0));
                        if (inner->number == 1) slot = val;
                        else if (inner->number == 2) j_id = val;
                    }
                    if (j_id > 0) hero.artifact.jewels[slot] = j_id;
                    break;
                }
                case 45: {
                    if (field->wire_type != 2) {
                        reader.skip(field->wire_type);
                        break;
                    }
                    proto::Reader entry{reader.bytes().value_or(std::span<const std::uint8_t>{})};
                    std::int32_t p_slot = 0, p_type = 0, p_val = 0;
                    while (!entry.eof()) {
                        const auto inner = entry.field();
                        if (!inner || inner->wire_type != 0) break;
                        const auto val = static_cast<std::int32_t>(entry.varint().value_or(0));
                        if (inner->number == 1) p_slot = val;
                        else if (inner->number == 2) p_type = val;
                        else if (inner->number == 3) p_val = val;
                    }
                    if (p_slot > 0 && p_type > 0) hero.artifact.properties[p_slot] = {p_type, p_val};
                    break;
                }
                case 8:
                case 38: {
                    if (field->wire_type != 2) {
                        reader.skip(field->wire_type);
                        break;
                    }
                    proto::Reader entry{reader.bytes().value_or(std::span<const std::uint8_t>{})};
                    std::int32_t s_id = 0, s_lvl = 0;
                    while (!entry.eof()) {
                        const auto inner = entry.field();
                        if (!inner || inner->wire_type != 0) break;
                        const auto val = static_cast<std::int32_t>(entry.varint().value_or(0));
                        if (inner->number == 1) s_id = val;
                        else if (inner->number == 2) s_lvl = val;
                    }
                    if (s_id > 0 && s_lvl > 0) hero.skills[s_id] = s_lvl;
                    break;
                }
                case 14:
                case 39: {
                    if (field->wire_type != 2) {
                        reader.skip(field->wire_type);
                        break;
                    }
                    proto::Reader entry{reader.bytes().value_or(std::span<const std::uint8_t>{})};
                    std::int32_t f_id = 0, f_status = 0;
                    while (!entry.eof()) {
                        const auto inner = entry.field();
                        if (!inner || inner->wire_type != 0) break;
                        const auto val = static_cast<std::int32_t>(entry.varint().value_or(0));
                        if (inner->number == 1) f_id = val;
                        else if (inner->number == 2) f_status = val;
                    }
                    if (f_id > 0 && f_status > 0) hero.archives[f_id] = f_status;
                    break;
                }
                case 12:
                case 40: {
                    if (field->wire_type != 2) {
                        reader.skip(field->wire_type);
                        break;
                    }
                    proto::Reader entry{reader.bytes().value_or(std::span<const std::uint8_t>{})};
                    std::int32_t pos_id = 0, f_lv = 0;
                    while (!entry.eof()) {
                        const auto inner = entry.field();
                        if (!inner || inner->wire_type != 0) break;
                        const auto val = static_cast<std::int32_t>(entry.varint().value_or(0));
                        if (inner->number == 1) pos_id = val;
                        else if (inner->number == 2) f_lv = val;
                    }
                    if (pos_id > 0 && f_lv > 0) hero.fetters[pos_id] = f_lv;
                    break;
                }
                case 41: {
                    const auto dub_id = static_cast<std::int32_t>(reader.varint().value_or(0));
                    if (dub_id > 0 && !std::ranges::contains(hero.dubbings, dub_id)) hero.dubbings.push_back(dub_id);
                    break;
                }
                case 42: hero.battle_skin = static_cast<std::int32_t>(reader.varint().value_or(0)); break;
                case 43: hero.outer_skin = static_cast<std::int32_t>(reader.varint().value_or(0)); break;
                default: reader.skip(field->wire_type); break;
            }
        }
        out.push_back(std::move(hero));
    }
    return out;
}

std::vector<AccountItem> decode_items(const std::vector<std::uint8_t>& blob) {
    std::vector<AccountItem> out;
    std::size_t pos = 0;
    while (pos < blob.size()) {
        std::uint32_t size = 0;
        if (!read_u32(blob, pos, size) || pos + size > blob.size()) break;
        proto::Reader reader{{blob.data() + pos, size}};
        pos += size;
        AccountItem item;
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            switch (field->number) {
                case 1: item.id = static_cast<std::int32_t>(reader.varint().value_or(0)); break;
                case 2: item.num = static_cast<std::int32_t>(reader.varint().value_or(0)); break;
                case 3: item.locked = reader.varint().value_or(0) != 0; break;
                case 4: item.day_get = static_cast<std::int32_t>(reader.varint().value_or(0)); break;
                default: reader.skip(field->wire_type); break;
            }
        }
        out.push_back(std::move(item));
    }
    return out;
}

} // namespace

std::vector<std::uint8_t> encode_player(const AccountPlayer& p) {
    proto::Writer w;
    w.int32(1, p.level);
    w.int32(2, p.exp);
    w.int32(3, p.gold);
    w.int32(4, p.crystal);
    w.int32(5, p.jewel_chip);
    w.int32(6, p.equip_exp);
    w.int32(7, p.power);
    w.int32(8, p.main_chapter);
    w.int32(9, p.main_section);
    w.string(10, p.nickname);
    w.int32(11, p.hero_exp);
    for (const auto id : p.cleared_main) w.int32(12, id);
    for (const auto& [group, state] : p.guides) {
        proto::Writer entry;
        entry.int32(1, group);
        entry.int32(2, state);
        w.message(13, entry.data());
    }
    for (const auto& e : p.equips) {
        proto::Writer entry;
        entry.int32(1, e.id);
        entry.int32(2, e.type_id);
        entry.int32(3, e.level);
        entry.int32(5, e.star);
        for (const auto& [at, av] : e.attrs) {
            proto::Writer attr;
            attr.int32(1, at);
            attr.int32(2, av);
            entry.message(6, attr.data());
        }
        w.message(14, entry.data());
    }
    w.int32(15, p.pending_section);
    w.int32(16, p.pending_chapter);
    w.int32(17, p.skill_point);
    w.int32(19, p.rewarded_level);
    for (const auto id : p.collection_awards) w.int32(20, id);
    w.int32(21, p.daily_activity);
    for (const auto id : p.daily_tasks) w.int32(22, id);
    for (const auto id : p.daily_boxes) w.int32(23, id);
    for (const auto id : p.challenge_tasks) w.int32(25, id);
    for (const auto id : p.challenge_boxes) w.int32(26, id);
    for (const auto& plan : p.equip_plans) {
        proto::Writer entry;
        entry.int32(1, plan.id);
        entry.string(2, plan.name);
        for (const auto& [slot, equip_id] : plan.positions) {
            proto::Writer pos;
            pos.int32(1, slot);
            pos.int32(2, equip_id);
            entry.message(3, pos.data());
        }
        w.message(27, entry.data());
    }
    for (const auto id : p.cleared_equip_plans) w.int32(28, id);
    w.int32(29, p.sign_count);
    w.int32(30, p.last_sign_time);
    w.int64(31, p.sign_start);
    w.int32(32, p.divination_check);
    w.int32(33, p.divination_stamp);
    w.int32(34, p.birthday);
    w.int32(35, p.divination_time);
    w.int32(36, p.power_of_light);
    w.int32(37, p.vow_coin);
    w.int32(38, p.wish_crystal);
    for (const auto& [id, pool] : p.draw_pools) {
        proto::Writer entry;
        entry.int32(1, id);
        entry.int32(2, pool.one);
        entry.int32(3, pool.ten);
        entry.int32(4, pool.since_security);
        entry.int32(5, pool.since_top);
        for (const auto& record : pool.history) {
            proto::Writer r;
            r.int32(1, record.item_id);
            r.int32(2, record.num);
            r.boolean(3, record.trans);
            r.int64(4, record.time);
            entry.message(6, r.data());
        }
        w.message(39, entry.data());
    }
    for (const auto level : p.course_levels) w.int32(40, level);
    for (const auto& [id, count] : p.daily_progress) {
        proto::Writer entry;
        entry.int32(1, id);
        entry.int32(2, count);
        w.message(41, entry.data());
    }
    w.int32(42, p.task_day);
    w.int32(52, p.show_hero);   // BaseInfo.Show
    w.int32(53, p.icon_id);     // IconInfo.iconID
    w.int32(54, p.ornament_id); // IconInfo.ornamentID
    w.int32(55, p.touch_day);   // 触摸计数 game day
    w.int32(56, p.touch_total); // 当日触摸总次数
    for (const auto& [hid, cnt] : p.touch_hero) {
        proto::Writer entry;
        entry.int32(1, hid);
        entry.int32(2, cnt);
        w.message(57, entry.data());
    }
    w.int32(58, p.stat_gift_given); // 累计送礼次数 (成就计数)
    w.int32(59, p.stat_talk_given); // 累计交谈次数 (成就计数)
    for (const auto id : p.achv_claimed) w.int32(60, id);
    for (const auto idx : p.achv_point_claimed) w.int32(61, idx);
    for (const auto& [id, obtained] : p.medal_pack) {
        proto::Writer entry;
        entry.int32(1, id);
        entry.int64(2, obtained);
        w.message(62, entry.data());
    }
    for (const auto& [slot, id] : p.medal_show) {
        if (slot < 0 || slot > 2 || id <= 0) continue;
        proto::Writer entry;
        entry.int32(1, slot);
        entry.int32(2, id);
        w.message(63, entry.data());
    }
    for (const auto relic : p.relic_pack) w.int32(64, relic);
    w.int32(65, p.friend_coin);
    for (const auto& [blog_id, like_time] : p.npc_blog_likes) { // 66: 时光点赞
        proto::Writer entry;
        entry.int32(1, blog_id);
        entry.int32(2, like_time);
        w.message(66, entry.data());
    }
    for (const auto& [blog_id, replies] : p.npc_blog_replies) { // 67: 时光回复记录
        for (const auto& r : replies) {
            proto::Writer entry;
            entry.int32(1, blog_id);
            entry.int32(2, r.reply_id);
            entry.int32(3, r.time_offset);
            entry.int32(4, r.chat_group_id);
            w.message(67, entry.data());
        }
    }
    for (const auto& [id, level] : p.star_skills) {
        proto::Writer entry;
        entry.int32(1, id);
        entry.int32(2, level);
        w.message(24, entry.data());
    }
    for (const auto& mail : p.mails) {
        proto::Writer entry;
        entry.int64(1, mail.id);
        entry.int32(2, mail.item_id);
        entry.int32(3, mail.num);
        entry.int64(4, mail.time);
        entry.int32(5, mail.state);
        w.message(18, entry.data());
    }
    for (const auto& [id, ls] : p.college) { // 白夜行星建筑: {1 id, 2 level, 3 star}
        proto::Writer entry;
        entry.int32(1, id);
        entry.int32(2, ls.first);
        entry.int32(3, ls.second);
        w.message(43, entry.data());
    }
    if (p.build_queue.building_id > 0) { // 升级队列: 44 id, 45 endUnix, 46 duration
        w.int32(44, p.build_queue.building_id);
        w.int64(45, p.build_queue.end_unix);
        w.int32(46, p.build_queue.duration_sec);
    }
    w.int32(47, p.shop_refresh_times);
    w.int32(48, p.shop_refresh_day);
    for (const auto& [gid, cnt] : p.shop_goods_bought) {
        proto::Writer entry;
        entry.int32(1, gid);
        entry.int32(2, cnt);
        w.message(49, entry.data());
    }
    for (const auto gid : p.owned_skins) w.int32(50, gid);
    w.int32(51, p.skin_coupon);
    return w.data();
}

AccountPlayer decode_player(std::span<const std::uint8_t> blob) {
    AccountPlayer p;
    if (!blob.empty()) p.main_section = 1;
    proto::Reader reader{blob};
    while (!reader.eof()) {
        const auto field = reader.field();
        if (!field) break;
        if (field->number == 10 && field->wire_type == 2) {
            p.nickname = reader.string().value_or(p.nickname);
            continue;
        }
        if (field->number == 13 && field->wire_type == 2) {
            proto::Reader entry{reader.bytes().value_or(std::span<const std::uint8_t>{})};
            std::int32_t group = 0, state = 0;
            while (!entry.eof()) {
                const auto f = entry.field();
                if (!f || f->wire_type != 0) break;
                const auto v = static_cast<std::int32_t>(entry.varint().value_or(0));
                if (f->number == 1) group = v;
                else if (f->number == 2) state = v;
            }
            if (group > 0) p.guides[group] = state;
            continue;
        }
        if ((field->number == 24 || field->number == 41) && field->wire_type == 2) {
            proto::Reader entry{reader.bytes().value_or(std::span<const std::uint8_t>{})};
            std::int32_t id = 0, level = 0;
            while (!entry.eof()) {
                const auto f = entry.field();
                if (!f || f->wire_type != 0) break;
                const auto v = static_cast<std::int32_t>(entry.varint().value_or(0));
                if (f->number == 1) id = v;
                else if (f->number == 2) level = v;
            }
            if (id > 0) (field->number == 24 ? p.star_skills : p.daily_progress)[id] = level;
            continue;
        }
        if (field->number == 39 && field->wire_type == 2) {
            proto::Reader entry{reader.bytes().value_or(std::span<const std::uint8_t>{})};
            std::int32_t id = 0;
            AccountDrawPool pool;
            while (!entry.eof()) {
                const auto f = entry.field();
                if (f && f->number == 6 && f->wire_type == 2) {
                    proto::Reader r{entry.bytes().value_or(std::span<const std::uint8_t>{})};
                    AccountDrawRecord record;
                    while (!r.eof()) {
                        const auto rf = r.field();
                        if (!rf || rf->wire_type != 0) break;
                        const auto rv = r.varint().value_or(0);
                        if (rf->number == 1) record.item_id = static_cast<std::int32_t>(rv);
                        else if (rf->number == 2) record.num = static_cast<std::int32_t>(rv);
                        else if (rf->number == 3) record.trans = rv != 0;
                        else if (rf->number == 4) record.time = static_cast<std::int64_t>(rv);
                    }
                    pool.history.push_back(record);
                    continue;
                }
                if (!f || f->wire_type != 0) break;
                const auto v = static_cast<std::int32_t>(entry.varint().value_or(0));
                if (f->number == 1) id = v;
                else if (f->number == 2) pool.one = v;
                else if (f->number == 3) pool.ten = v;
                else if (f->number == 4) pool.since_security = v;
                else if (f->number == 5) pool.since_top = v;
            }
            if (id > 0) p.draw_pools[id] = pool;
            continue;
        }
        if (field->number == 27 && field->wire_type == 2) {
            proto::Reader entry{reader.bytes().value_or(std::span<const std::uint8_t>{})};
            AccountEquipPlan plan;
            while (!entry.eof()) {
                const auto inner = entry.field();
                if (!inner) break;
                if (inner->number == 1 && inner->wire_type == 0) plan.id = static_cast<std::int32_t>(entry.varint().value_or(0));
                else if (inner->number == 2 && inner->wire_type == 2) plan.name = entry.string().value_or("");
                else if (inner->number == 3 && inner->wire_type == 2) {
                    proto::Reader pos{entry.bytes().value_or(std::span<const std::uint8_t>{})};
                    std::int32_t slot = 0, equip_id = 0;
                    while (!pos.eof()) {
                        const auto f = pos.field();
                        if (!f || f->wire_type != 0) break;
                        const auto value = static_cast<std::int32_t>(pos.varint().value_or(0));
                        if (f->number == 1) slot = value;
                        else if (f->number == 2) equip_id = value;
                    }
                    if (equip_id > 0) plan.positions[slot] = equip_id;
                } else if (!entry.skip(inner->wire_type)) {
                    break;
                }
            }
            if (plan.id > 0) p.equip_plans.push_back(std::move(plan));
            continue;
        }
        if (field->number == 14 && field->wire_type == 2) {
            proto::Reader entry{reader.bytes().value_or(std::span<const std::uint8_t>{})};
            AccountEquip equip;
            while (!entry.eof()) {
                const auto f = entry.field();
                if (!f || f->wire_type != 0) {
                    if (f && !entry.skip(f->wire_type)) break;
                    continue;
                }
                const auto v = static_cast<std::int32_t>(entry.varint().value_or(0));
                if (f->number == 1) equip.id = v;
                else if (f->number == 2) equip.type_id = v;
                else if (f->number == 3) equip.level = v;
                else if (f->number == 5) equip.star = v;
                else if (f->number == 6 && f->wire_type == 2) {
                    proto::Reader attr{entry.bytes().value_or(std::span<const std::uint8_t>{})};
                    std::int32_t at = 0, av = 0;
                    while (!attr.eof()) {
                        const auto af = attr.field();
                        if (!af || af->wire_type != 0) {
                            if (af && !attr.skip(af->wire_type)) break;
                            continue;
                        }
                        const auto avv = static_cast<std::int32_t>(attr.varint().value_or(0));
                        if (af->number == 1) at = avv;
                        else if (af->number == 2) av = avv;
                    }
                    if (at > 0) equip.attrs[at] = av;
                }
            }
            if (equip.id > 0) p.equips.push_back(equip);
            continue;
        }
        if (field->number == 57 && field->wire_type == 2) {
            proto::Reader entry{reader.bytes().value_or(std::span<const std::uint8_t>{})};
            std::int32_t hid = 0, cnt = 0;
            while (!entry.eof()) {
                const auto f = entry.field();
                if (!f || f->wire_type != 0) {
                    if (f && !entry.skip(f->wire_type)) break;
                    continue;
                }
                const auto v = static_cast<std::int32_t>(entry.varint().value_or(0));
                if (f->number == 1) hid = v;
                else if (f->number == 2) cnt = v;
            }
            if (hid > 0 && cnt > 0) p.touch_hero[hid] = cnt;
            continue;
        }
        if (field->number == 62 && field->wire_type == 2) {
            proto::Reader entry{reader.bytes().value_or(std::span<const std::uint8_t>{})};
            std::int32_t id = 0;
            std::int64_t obtained = 0;
            while (!entry.eof()) {
                const auto f = entry.field();
                if (!f) break;
                if (f->wire_type == 0) {
                    const auto v = entry.varint().value_or(0);
                    if (f->number == 1) id = static_cast<std::int32_t>(v);
                    else if (f->number == 2) obtained = static_cast<std::int64_t>(v);
                } else if (!entry.skip(f->wire_type)) {
                    break;
                }
            }
            if (id > 0 && obtained > 0) p.medal_pack[id] = obtained;
            continue;
        }
        if (field->number == 63 && field->wire_type == 2) {
            proto::Reader entry{reader.bytes().value_or(std::span<const std::uint8_t>{})};
            std::int32_t slot = -1, id = 0;
            while (!entry.eof()) {
                const auto f = entry.field();
                if (!f || f->wire_type != 0) {
                    if (f && !entry.skip(f->wire_type)) break;
                    continue;
                }
                const auto v = static_cast<std::int32_t>(entry.varint().value_or(0));
                if (f->number == 1) slot = v;
                else if (f->number == 2) id = v;
            }
            if (slot >= 0 && slot <= 2 && id > 0) p.medal_show[slot] = id;
            continue;
        }
        if (field->number == 66 && field->wire_type == 2) { // 时光点赞: {1 blogID, 2 likeTime}
            proto::Reader entry{reader.bytes().value_or(std::span<const std::uint8_t>{})};
            std::int32_t blog_id = 0, like_time = 0;
            while (!entry.eof()) {
                const auto f = entry.field();
                if (!f || f->wire_type != 0) {
                    if (f && !entry.skip(f->wire_type)) break;
                    continue;
                }
                const auto v = static_cast<std::int32_t>(entry.varint().value_or(0));
                if (f->number == 1) blog_id = v;
                else if (f->number == 2) like_time = v;
            }
            if (blog_id > 0) p.npc_blog_likes[blog_id] = like_time;
            continue;
        }
        if (field->number == 67 && field->wire_type == 2) { // 时光回复: {1 blogID, 2 replyID, 3 time, 4 chatGroupID}
            proto::Reader entry{reader.bytes().value_or(std::span<const std::uint8_t>{})};
            AccountPlayer::NpcBlogReply r;
            std::int32_t blog_id = 0;
            while (!entry.eof()) {
                const auto f = entry.field();
                if (!f || f->wire_type != 0) {
                    if (f && !entry.skip(f->wire_type)) break;
                    continue;
                }
                const auto v = static_cast<std::int32_t>(entry.varint().value_or(0));
                if (f->number == 1) blog_id = v;
                else if (f->number == 2) r.reply_id = v;
                else if (f->number == 3) r.time_offset = v;
                else if (f->number == 4) r.chat_group_id = v;
            }
            if (blog_id > 0 && r.reply_id > 0) p.npc_blog_replies[blog_id].push_back(r);
            continue;
        }
        if (field->number == 43 && field->wire_type == 2) {
            proto::Reader entry{reader.bytes().value_or(std::span<const std::uint8_t>{})};
            std::int32_t id = 0, level = 0, star = 0;
            while (!entry.eof()) {
                const auto f = entry.field();
                if (!f || f->wire_type != 0) {
                    if (f && !entry.skip(f->wire_type)) break;
                    continue;
                }
                const auto v = static_cast<std::int32_t>(entry.varint().value_or(0));
                if (f->number == 1) id = v;
                else if (f->number == 2) level = v;
                else if (f->number == 3) star = v;
            }
            if (id > 0) p.college[id] = {level, star};
            continue;
        }
        if (field->number == 18 && field->wire_type == 2) {
            proto::Reader entry{reader.bytes().value_or(std::span<const std::uint8_t>{})};
            AccountMail mail;
            while (!entry.eof()) {
                const auto f = entry.field();
                if (!f || f->wire_type != 0) {
                    if (f && !entry.skip(f->wire_type)) break;
                    continue;
                }
                const auto v = entry.varint().value_or(0);
                if (f->number == 1) mail.id = static_cast<std::int64_t>(v);
                else if (f->number == 2) mail.item_id = static_cast<std::int32_t>(v);
                else if (f->number == 3) mail.num = static_cast<std::int32_t>(v);
                else if (f->number == 4) mail.time = static_cast<std::int64_t>(v);
                else if (f->number == 5) mail.state = static_cast<std::int32_t>(v);
            }
            if (mail.id > 0) p.mails.push_back(mail);
            continue;
        }
        if (field->number == 49 && field->wire_type == 2) {
            proto::Reader entry{reader.bytes().value_or(std::span<const std::uint8_t>{})};
            std::int32_t gid = 0, cnt = 0;
            while (!entry.eof()) {
                const auto f = entry.field();
                if (!f || f->wire_type != 0) {
                    if (f && !entry.skip(f->wire_type)) break;
                    continue;
                }
                const auto v = static_cast<std::int32_t>(entry.varint().value_or(0));
                if (f->number == 1) gid = v;
                else if (f->number == 2) cnt = v;
            }
            if (gid > 0) p.shop_goods_bought[gid] = cnt;
            continue;
        }
        const auto value = static_cast<std::int32_t>(reader.varint().value_or(0));
        switch (field->number) {
            case 1: p.level = value; break;
            case 2: p.exp = value; break;
            case 3: p.gold = value; break;
            case 4: p.crystal = value; break;
            case 5: p.jewel_chip = value; break;
            case 6: p.equip_exp = value; break;
            case 7: p.power = value; break;
            case 8: p.main_chapter = value; break;
            case 9: p.main_section = value; break;
            case 11: p.hero_exp = value; break;
            case 12: p.cleared_main.push_back(value); break;
            case 15: p.pending_section = value; break;
            case 16: p.pending_chapter = value; break;
            case 17: p.skill_point = value; break;
            case 19: p.rewarded_level = value; break;
            case 20: p.collection_awards.push_back(value); break;
            case 21: p.daily_activity = value; break;
            case 22: p.daily_tasks.push_back(value); break;
            case 23: p.daily_boxes.push_back(value); break;
            case 25: p.challenge_tasks.push_back(value); break;
            case 26: p.challenge_boxes.push_back(value); break;
            case 28: p.cleared_equip_plans.push_back(value); break;
            case 29: p.sign_count = value; break;
            case 30: p.last_sign_time = value; break;
            case 31: p.sign_start = value; break;
            case 32: p.divination_check = value; break;
            case 33: p.divination_stamp = value; break;
            case 34: p.birthday = value; break;
            case 35: p.divination_time = value; break;
            case 36: p.power_of_light = value; break;
            case 37: p.vow_coin = value; break;
            case 38: p.wish_crystal = value; break;
            case 40: p.course_levels.push_back(value); break;
            case 42: p.task_day = value; break;
            case 52: p.show_hero = value; break;
            case 53: p.icon_id = value; break;
            case 54: p.ornament_id = value; break;
            case 55: p.touch_day = value; break;
            case 56: p.touch_total = value; break;
            case 58: p.stat_gift_given = value; break;
            case 59: p.stat_talk_given = value; break;
            case 60: p.achv_claimed.push_back(value); break;
            case 61: p.achv_point_claimed.push_back(value); break;
            case 64:
                if (value > 0 && !std::ranges::contains(p.relic_pack, value)) p.relic_pack.push_back(value);
                break;
            case 65: p.friend_coin = value; break;
            case 44: p.build_queue.building_id = value; break;
            case 45: p.build_queue.end_unix = static_cast<std::int64_t>(reader.varint().value_or(0)); break;
            case 46: p.build_queue.duration_sec = value; break;
            case 47: p.shop_refresh_times = value; break;
            case 48: p.shop_refresh_day = value; break;
            case 50: p.owned_skins.push_back(value); break;
            case 51: p.skin_coupon = value; break;
            default: break;
        }
    }
    return p;
}

namespace {

void persist_account(std::string_view token, const Account& account) {
    persist::AccountRow row;
    row.token = std::string{token};
    row.id = account.id;
    row.login_count = account.login_count;
    row.is_create_role = account.is_create_role ? 1 : 0;
    row.heroes = encode_heroes(account.heroes);
    row.items = encode_items(account.items);
    row.player = encode_player(account.player);
    persist::save_account(row);
}

bool has_progress(const AccountPlayer& p) {
    return !p.cleared_main.empty() || p.main_section > 1 ||
           std::ranges::any_of(p.guides, [](const auto& e) { return e.second != 0; });
}

} // namespace

void AccountRepository::hydrate() {
    for (auto& row : persist::load_accounts()) {
        if (accounts_.contains(row.token)) continue;
        Account account;
        account.id = row.id;
        account.login_count = row.login_count;
        account.is_create_role = row.is_create_role != 0;
        account.heroes = decode_heroes(row.heroes);
        account.items = decode_items(row.items);
        account.player = decode_player(row.player);
        const auto before = account.heroes.size();
        std::erase_if(account.heroes, [](const AccountHero& hero) { return hero.state == 1; });
        bool dirty = account.heroes.size() != before;
        const auto cap = max_hero_star(tables_.get());
        for (auto& hero : account.heroes) {
            const auto stage = default_hero_star(tables_.get(), hero.id);
            if (hero.star < stage) {
                hero.star = stage;
                dirty = true;
            }
            if (hero.star > cap) {
                hero.star = cap;
                dirty = true;
            }
        }
        if (account.player.rewarded_level < account.player.level) {
            const auto from = account.player.rewarded_level;
            grant_role_level_gifts(account, tables_.get(), from, account.player.level);
            account.player.rewarded_level = account.player.level;
            dirty = true;
        }
        for (auto it = account.items.begin(); it != account.items.end(); ) {
            if (const auto enu = item_currency_enu(tables_.get(), it->id)) {
                if (auto* field = wallet(account.player, *enu)) {
                    *field = clamp_add(*field, it->num);
                }
                it = account.items.erase(it);
                dirty = true;
            } else {
                ++it;
            }
        }
        // 贝黑莫斯
        if (!std::ranges::contains(account.heroes, 1003, &AccountHero::id)) {
            account.heroes.push_back(AccountHero{
                .id = 1003,
                .state = 2,
                .level = kDefaultHeroLevel,
                .star = default_hero_star(tables_.get(), 1003),
                .exp = 0,
                .get_time = 0,
                .name = {},
            });
            dirty = true;
        }
        // 保证兽魂 90
        if (account.player.equip_exp < 90) {
            account.player.equip_exp = 90;
            dirty = true;
        }
        // 白皇后与夏日花火直接赠送
        if (!std::ranges::contains(account.items, 1220803, &AccountItem::id)) {
            account.items.push_back(AccountItem{.id = 1220803, .num = 1, .locked = false});
            dirty = true;
        }
        if (!std::ranges::contains(account.player.owned_skins, 1980005)) {
            account.player.owned_skins.push_back(1980005);
            dirty = true;
        }
        if (!std::ranges::contains(account.items, 1221903, &AccountItem::id)) {
            account.items.push_back(AccountItem{.id = 1221903, .num = 1, .locked = false});
            dirty = true;
        }
        if (!std::ranges::contains(account.player.owned_skins, 1980004)) {
            account.player.owned_skins.push_back(1980004);
            dirty = true;
        }
        if (dirty) persist_account(row.token, account);
        accounts_.emplace(row.token, std::move(account));
    }

}

void AccountRepository::set_tables(std::shared_ptr<TableBlob> tables) {
    tables_ = std::move(tables);
    hydrate();
}

Account AccountRepository::build_fresh(std::string_view) const {
    Account account;
    account.login_count = 1;
    account.is_create_role = true;

    if (tables_) {
        GameTable unit_base;
        if (unit_base.load(*tables_, "UnitBase")) {
            for (const auto& row : unit_base.rows()) {
                const auto unit_type = unit_base.int_field(row, 20);
                if (unit_type && *unit_type == 1) {
                    AccountHero hero{
                        .id = static_cast<std::int32_t>(row.key),
                        .state = 2,
                        .level = kDefaultHeroLevel,
                        .star = default_hero_star(tables_.get(), static_cast<std::int32_t>(row.key)),
                        .exp = 0,
                        .get_time = 0,
                        .name = {},
                    };
                    account.heroes.push_back(hero);
                    break;
                }
            }
        }
    }
    if (!std::ranges::contains(account.heroes, 1003, &AccountHero::id)) {
        account.heroes.push_back(AccountHero{
            .id = 1003,
            .state = 2,
            .level = kDefaultHeroLevel,
            .star = default_hero_star(tables_.get(), 1003),
            .exp = 0,
            .get_time = 0,
            .name = {},
        });
    }

    account.player.gold = 6;
    account.player.crystal = 3;
    account.player.equip_exp = 90;
    for (const auto material : {1237801, 1237802, 1237803, 1237804, 1237805, 1237806}) {
        account.items.push_back(AccountItem{.id = material, .num = 999, .locked = false});
    }
    account.items.push_back(AccountItem{.id = 1220803, .num = 1, .locked = false});
    account.items.push_back(AccountItem{.id = 1221903, .num = 1, .locked = false});
    account.player.owned_skins.push_back(1980005);
    account.player.owned_skins.push_back(1980004);
    return account;
}

Account AccountRepository::create_or_get(std::string_view account, std::string_view token) {
    std::lock_guard lock{mutex_};
    hydrate();
    if (const auto it = accounts_.find(account); it != accounts_.end()) {
        Account result = it->second;
        result.login_count += 1;
        result.is_create_role = false;
        it->second = result;
        persist_account(account, result);
        return result;
    }
    const auto best = std::ranges::max_element(accounts_, [](const auto& a, const auto& b) {
        return a.second.player.cleared_main.size() < b.second.player.cleared_main.size();
    });
    if (best != accounts_.end() && has_progress(best->second.player)) {
        Account result = best->second;
        result.login_count += 1;
        result.is_create_role = false;
        accounts_.erase(best);
        accounts_.insert_or_assign(std::string{account}, result);
        persist_account(account, result);
        return result;
    }
    Account result = build_fresh(account);
    accounts_.emplace(std::string{account}, result);
    persist_account(account, result);
    return result;
}

void AccountRepository::save(std::string_view account, const Account& state) {
    std::lock_guard lock{mutex_};
    accounts_.insert_or_assign(std::string{account}, state);
    persist_account(account, state);
}

std::optional<Account> AccountRepository::find(std::string_view account) const {
    std::lock_guard lock{mutex_};
    const auto it = accounts_.find(account);
    if (it == accounts_.end()) return std::nullopt;
    return it->second;
}

} // namespace x2::offline
