#include "offline/protocol_builders.hpp"

#include <algorithm>
#include <ctime>
#include <random>
#include <ranges>
#include <set>

#include "offline/game_table.hpp"
#include "offline/mission.hpp"
#include "offline/shop.hpp"
#include "proto/protobuf.hpp"
#include "server/protocol_ids.hpp"

namespace x2::offline {

namespace {
using proto::Writer;


std::int32_t maze_price(const GameTable& item, const TableRow& row) {
    const auto type = item.int_field(row, 6).value_or(0);
    if (type == 5) return item.string_field(row, 24).value_or("").starts_with("ItemInGame/") ? 50 : 0;
    if (type != 4) return 0;
    switch (item.int_field(row, 7).value_or(0)) {
    case 2: return 100;
    case 3: return 150;
    case 4: return 200;
    case 5: return 300;
    case 8: return 400;
    default: return 0;
    }
}
} // namespace

std::vector<std::uint8_t> ProtocolBuilders::item_data(const AccountItem& item) {
    Writer writer;
    writer.int32(1, item.id);
    writer.int32(2, item.num);
    writer.boolean(3, item.locked);
    writer.int32(4, item.day_get);
    return writer.data();
}

std::vector<std::uint8_t> ProtocolBuilders::hero_equip(const AccountEquip& equip) {
    Writer writer;
    writer.int32(1, equip.id);
    writer.int32(2, equip.type_id);
    writer.int32(3, equip.level);
    writer.int32(5, equip.star);
    Writer param;
    if (!equip.attrs.empty()) {
        std::int32_t slot = 1;
        for (const auto& [at, av] : equip.attrs) {
            if (slot > 6) break;
            param.int32(static_cast<std::uint32_t>(slot * 2 - 1), at);
            param.int32(static_cast<std::uint32_t>(slot * 2), av);
            ++slot;
        }
    }
    writer.message(7, param.data());
    return writer.data();
}

std::vector<std::uint8_t> ProtocolBuilders::hero_data(const AccountHero& hero, const TableBlob* tables) {
    Writer writer;
    writer.int32(1, hero.id);
    writer.int32(2, hero.state == 0 ? 2 : hero.state);
    writer.int32(3, hero.level);
    writer.int32(4, hero.star);
    {
        Writer god;
        std::int32_t weapon_id = hero.artifact.id;
        if (weapon_id == 0 && tables) {
            GameTable attrib;
            if (attrib.load(*tables, "PlayerAttrib")) {
                if (const auto row = attrib.row(static_cast<std::uint64_t>(hero.id))) {
                    weapon_id = attrib.int_field(*row, 13).value_or(0);
                }
            }
        }
        if (weapon_id > 0) god.int32(1, weapon_id);
        god.int32(2, hero.artifact.level);
        god.int32(3, hero.artifact.star);
        for (const auto& [slot, j_id] : hero.artifact.jewels) {
            if (j_id > 0) {
                Writer pair;
                pair.int32(1, slot);
                pair.int32(2, j_id);
                god.message(4, pair.data());
            }
        }
        for (const auto slot : hero.god_slots) {
            Writer info; // GodSlotLockInfo: 1 slot, 2 state
            info.int32(1, slot);
            info.int32(2, 1);
            god.message(8, info.data());
        }
        if (tables && weapon_id > 0) {
            GameTable artifact_base;
            if (artifact_base.load(*tables, "ArtifactBase")) {
                if (const auto arow = artifact_base.row(static_cast<std::uint64_t>(weapon_id))) {
                    Writer attr;
                    const auto add_kv = [&](std::int32_t k, std::int64_t v) {
                        if (k <= 0 || v <= 0) return;
                        Writer kv;
                        kv.int32(1, k);
                        kv.int64(2, v);
                        attr.message(1, kv.data());
                    };
                    const auto star_idx = std::clamp(hero.artifact.star, 0, 6);
                    add_kv(artifact_base.int_field(*arow, 9).value_or(0),
                           artifact_base.int_field(*arow, 10).value_or(0));  // ArtifactAttr1/Value1
                    add_kv(artifact_base.int_field(*arow, 11).value_or(0),
                           artifact_base.int_field(*arow, 12).value_or(0)); // ArtifactAttr2/Value2
                    add_kv(artifact_base.int_field(*arow, 13).value_or(0),
                           artifact_base.int_field(*arow, 14).value_or(0)); // ArtifactAttr3/Value3
                    const auto sen1 = artifact_base.ints_field(*arow, 16);  // SenAttrValue1
                    if (star_idx < static_cast<std::int32_t>(sen1.size())) {
                        add_kv(artifact_base.int_field(*arow, 15).value_or(0), sen1[static_cast<std::size_t>(star_idx)]);
                    }
                    const auto sen2 = artifact_base.ints_field(*arow, 18);  // SenAttrValue2
                    if (star_idx < static_cast<std::int32_t>(sen2.size())) {
                        add_kv(artifact_base.int_field(*arow, 17).value_or(0), sen2[static_cast<std::size_t>(star_idx)]);
                    }
                    god.message(5, attr.data());
                }
            }
        }
        writer.message(5, god.data());
    }
    writer.int64(7, hero.exp);
    for (const auto& [slot, equip_id] : hero.worn) {
        Writer pair;
        pair.int32(1, slot);
        pair.int32(2, equip_id);
        writer.message(6, pair.data());
    }

    int skills = 0;
    std::set<std::int32_t> emitted_skills;
    if (tables) {
        GameTable unit;
        GameTable skill_base;
        const bool have_skill_base = skill_base.load(*tables, "SkillBase");
        if (unit.load(*tables, "UnitBase")) {
            if (const auto row = unit.row(static_cast<std::uint64_t>(hero.id))) {
                for (const auto skill_id : unit.ints_field(*row, 22)) {
                    if (skill_id <= 0) continue;

                    if (have_skill_base && !hero.god_shed) {
                        if (const auto skill_row = skill_base.row(static_cast<std::uint64_t>(skill_id))) {
                            if (skill_base.int_field(*skill_row, 13).value_or(0) == 8) continue;
                        }
                    }
                    Writer skill; // HeroSkill: 1 id, 2 level
                    skill.int32(1, skill_id);
                    const auto it = hero.skills.find(skill_id);
                    const int level = (it != hero.skills.end() && it->second > 0) ? it->second : 1;
                    skill.int32(2, level);
                    writer.message(8, skill.data());
                    emitted_skills.insert(skill_id);
                    ++skills;
                }
            }
        }
    }
    for (const auto& [skill_id, level] : hero.skills) {
        if (skill_id <= 0 || emitted_skills.contains(skill_id)) continue;
        Writer skill;
        skill.int32(1, skill_id);
        skill.int32(2, level > 0 ? level : 1);
        writer.message(8, skill.data());
        ++skills;
    }
    if (skills == 0) {
        Writer skill;
        skill.int32(1, 0);
        skill.int32(2, 1);
        writer.message(8, skill.data());
    }
    writer.string(11, hero.name);

    for (const auto& [pos_id, lv] : hero.fetters) {
        if (pos_id <= 0) continue;
        proto::Writer fetter;
        fetter.int32(1, pos_id);
        fetter.int32(2, lv);
        writer.message(12, fetter.data());
    }

    writer.int64(13, hero.get_time);

    int archives_count = 0;
    const int cur_favor = hero.favor_level > 0 ? hero.favor_level : 1;
    if (tables) {
        GameTable files;
        if (files.load(*tables, "FavorabilityFiles")) {
            for (const auto& row : files.rows()) {
                const auto hid = files.int_field(row, 2).value_or(0);
                if (hid != hero.id) continue;
                const auto fid = files.int_field(row, 1).value_or(0);
                if (fid <= 0) continue;
                const auto trigger_type = files.int_field(row, 3).value_or(0);
                const auto type_num = files.int_field(row, 4).value_or(0);
                int status = 0; // Lock
                const auto it = hero.archives.find(fid);
                if (it != hero.archives.end()) {
                    status = it->second;
                } else if (trigger_type == 1 && cur_favor >= type_num) {
                    status = 1; // CanUnLock
                }
                Writer arch;
                arch.int32(1, fid);
                arch.int32(2, status);
                writer.message(14, arch.data());
                ++archives_count;
            }
        }
    }
    for (const auto& [fid, status] : hero.archives) {
        if (fid <= 0) continue;
        if (!tables) {
            Writer arch;
            arch.int32(1, fid);
            arch.int32(2, status);
            writer.message(14, arch.data());
            ++archives_count;
        }
    }
    if (archives_count == 0) {
        Writer arch;
        arch.int32(1, 0);
        arch.int32(2, 0);
        writer.message(14, arch.data());
    }

    return writer.data();
}

std::vector<std::uint8_t> ProtocolBuilders::hero_all(const Account& account, const TableBlob* tables) {
    Writer writer;
    for (const auto& hero : account.heroes) {
        writer.message(1, hero_data(hero, tables));
    }
    return writer.data();
}

std::vector<std::uint8_t> ProtocolBuilders::item_all(const Account& account) {
    Writer writer;
    for (const auto& item : account.items) {
        if (item.num <= 0) continue; // bag shows num >= 0, so a zero stack stays on screen
        writer.message(1, item_data(item));
    }
    return writer.data();
}

std::vector<std::uint8_t> ProtocolBuilders::equip_all(const Account& account) {
    Writer writer;
    for (const auto& equip : account.player.equips) writer.message(1, hero_equip(equip));
    return writer.data();
}

std::vector<std::uint8_t> ProtocolBuilders::fight_profile(const TableBlob* tables, const Account& account) {
    const auto section = account.player.pending_section;
    if (section <= 0) return {};
    std::int32_t chapter = account.player.pending_chapter;
    std::int32_t scene = 0;
    if (!tables) return {};
    GameTable sec, map;
    if (!sec.load(*tables, "SectionTable")) return {};
    const auto row = sec.row(static_cast<std::uint64_t>(section));
    if (!row) return {};
    if (sec.int_field(*row, 69).value_or(0) == 0) return {};
    if (chapter <= 0) chapter = sec.int_field(*row, 7).value_or(0);
    const auto maps = sec.ints_field(*row, 6);
    if (!maps.empty() && map.load(*tables, "MapInfo")) {
        if (const auto mrow = map.row(static_cast<std::uint64_t>(maps[0]))) {
            scene = map.int_field(*mrow, 8).value_or(0);
        }
    }
    Writer profile; // FightDataProfile declaration order
    profile.int32(1, section);
    profile.int32(2, 1); // layer
    Writer hero;         // FightHerosProfile: 1 heroId, 2 isLeader, 3 state
    hero.int32(1, account.heroes.empty() ? 1003 : account.heroes.front().id);
    hero.int32(2, 1);
    hero.int32(3, 1);
    profile.message(6, hero.data());
    profile.int32(7, chapter);
    profile.int32(12, 1); // version
    profile.int32(13, scene);
    profile.boolean(20, true); // isProfileValid
    return profile.data();
}

std::vector<std::uint8_t> ProtocolBuilders::login(Account& account, std::int32_t server_time,
                                                 const TableBlob* tables) {
    Writer writer;
    writer.int32(1, static_cast<std::int32_t>(server::LogicCode::Ok));
    writer.int64(2, account.id);
    writer.int32(3, account.login_count);
    writer.int32(5, 1);
    writer.int32(6, client_time(server_time));
    writer.boolean(7, account.is_create_role);
    writer.int32(8, static_cast<std::int32_t>(server::LogicCode::LoginCheckSuccess));
    writer.message(9, hero_all(account, tables));
    writer.message(10, item_all(account));
    writer.message(13, build_hero_skin_all(account, tables)); // Tag 13: heroSkinAll (L2C_HeroSkinAll)
    writer.message(14, growth_base(account, tables));
    if (tables) {
        writer.message(12, card_pool(*tables, account, server_time));
        writer.message(17, daily_task_reply(account, *tables));
        writer.message(19, challenge_task_reply(account, *tables));
    }
    writer.message(16, equip_all(account));
    writer.string(21, "1");
    return writer.data();
}

std::vector<std::int32_t> ProtocolBuilders::icon_unlock_ids(const AccountHero& hero,
                                                             const TableBlob* tables,
                                                             const Account* account) {
    std::vector<std::int32_t> ids;
    const auto base_pid = 1270000 + (hero.id - 1000) * 100;
    ids.push_back(base_pid + 1);
    if (!tables) return ids;
    GameTable fav_hero;
    if (fav_hero.load(*tables, "FavorabilityHero")) {
        if (const auto frow = fav_hero.row(static_cast<std::uint64_t>(hero.id))) {
            const auto award_level = fav_hero.int_field(*frow, 4).value_or(0);
            const auto picture_item = fav_hero.int_field(*frow, 5).value_or(0);
            const auto cur_favor = hero.favor_level > 0 ? hero.favor_level : 1;
            if (award_level > 0 && cur_favor >= award_level && picture_item > 0) ids.push_back(picture_item);
        }
    }
    if (account) {
        GameTable appearance, picture;
        const bool has_tables = appearance.load(*tables, "Appearance") && picture.load(*tables, "Picture");
        if (has_tables) {
            const auto skin_map = collect_hero_skins(*account, tables);
            const auto skin_it = skin_map.find(hero.id);
            if (skin_it != skin_map.end()) {
                for (const auto skin_id : skin_it->second) {
                    const auto arow = appearance.row(static_cast<std::uint64_t>(skin_id));
                    if (!arow) continue;
                    const auto icon_path = appearance.string_field(*arow, 9); // f9 Icon
                    if (!icon_path || icon_path->empty()) continue;
                    for (const auto& prow : picture.rows()) {
                        const auto drawing = picture.string_field(prow, 4); // f4 PictureDrawing
                        if (drawing && *drawing == *icon_path) {
                            if (!std::ranges::contains(ids, static_cast<std::int32_t>(prow.key))) {
                                ids.push_back(static_cast<std::int32_t>(prow.key));
                            }
                            break;
                        }
                    }
                }
            }
        }
    }
    return ids;
}

std::vector<std::uint8_t> ProtocolBuilders::growth_base(const Account& account, const TableBlob* tables) {

    Writer writer;

    writer.message(7, {});
    writer.message(8, {});
    if (account.player.build_queue.building_id > 0) {
        const auto& q = account.player.build_queue;
        Writer queue;
        queue.int32(1, q.building_id);
        queue.int32(2, 1);
        queue.int32(3, client_time(q.end_unix - q.duration_sec));
        queue.int32(4, client_time(q.end_unix));
        writer.message(10, queue.data());
    } else {
        writer.message(10, {});
    }
    writer.message(11, {}); 
    const auto state = [&account](std::int32_t id, std::int32_t def_level, std::int32_t def_star)
        -> std::pair<std::int32_t, std::int32_t> {
        const auto it = account.player.college.find(id);
        if (it != account.player.college.end()) return it->second;
        return {def_level, def_star};
    };
    const auto add_building = [&writer](std::uint32_t field, std::int32_t id, std::int32_t level,
                                        std::int32_t star) {
        Writer entry;
        entry.int32(1, id);
        entry.int32(2, level);
        entry.int32(3, star);
        writer.message(field, entry.data());
    };
    GameTable buildings;
    bool wrote = false;
    if (tables && buildings.load(*tables, "CollegeBuilding")) {
        for (const auto& row : buildings.rows()) {
            const auto id = static_cast<std::int32_t>(row.key);
            const auto open = buildings.int_field(row, 9).value_or(1); // f9 BuildingOpen
            if (id <= 0 || open != 1) continue;
            const auto type = buildings.int_field(row, 2).value_or(1); // f2 BaseType: 2 = wonder
            const auto [def_level, def_star] = state(id,
                                                     buildings.int_field(row, 10).value_or(1), // f10 InitialLevel
                                                     buildings.int_field(row, 11).value_or(0)); // f11 InitialStar
            add_building(type == 2 ? 9 : 2, id, def_level, def_star);
            wrote = true;
        }
    }
    if (!wrote) { // table unavailable: the fixed ids from CollegeBuilding
        for (const auto id : {701, 702, 703, 704, 705, 706, 707, 708}) {
            const auto [lv, star] = state(id, 1, 1);
            add_building(2, id, lv, star);
        }
        for (const auto id : {721, 722, 723, 724, 725, 726, 727, 728}) {
            const auto [lv, star] = state(id, 1, 0);
            add_building(9, id, lv, star);
        }
    }
    return writer.data();
}

std::vector<std::uint8_t> ProtocolBuilders::player_data(const Account& account, std::int32_t server_time,
                                                        const TableBlob* tables) {
    const auto& p = account.player;
    Writer base;
    base.int64(1, account.id);           // Id: MainHallFSM.NetSyncUpdate waits for Id >= 1
    base.string(2, p.nickname);          // NickName
    base.int32(3, p.level);              // Level
    base.int32(4, p.crystal);            // Crystal
    base.int32(5, p.gold);               // Gold
    base.int32(6, p.exp);                // Exp
    base.int32(7, p.equip_exp);          // EquipExp
    base.int32(10, p.jewel_chip);        // JewelChip ("Dust" in GMModule)
    base.int32(14, p.hero_exp);          // HeroExp
    base.int32(29, p.skill_point);       // StarSkillPoint (currency E_SkillPoint)
    for (const auto& [group, state] : p.guides) { // QuestIDs: ContainerIntIntProto{1 idx, 2 val}
        Writer entry;
        entry.int32(1, group);
        entry.int32(2, state);
        base.message(15, entry.data());
    }
    base.int64(18, server_time);         // RegistTime
    base.int64(19, server_time);         // LastLoginTime
    base.int32(20, p.daily_activity);    // DailyActivity: the daily-task bar
    base.int32(23, p.power_of_light);    // PowerOfLight 光能
    base.int32(24, p.sign_count);        // SignInCount
    base.int32(28, p.wish_crystal);      // WishCrystal 许愿水晶
    base.int32(35, p.vow_coin);          // VowOfCoin 许愿币
    base.int32(30, p.friend_coin); // FriendCoin 友情点 (GetCurrencyNum enu17 → BaseInfo.FriendCoin)
    base.int32(47, p.skin_coupon);       // SkinCoupon 外观券 (currency E_SkinTicket)
    if (p.last_sign_time > 0) base.int32(25, client_time(p.last_sign_time));
    if (p.sign_start > 0) base.int64(26, p.sign_start);
    if (p.birthday > 0) base.int32(32, p.birthday);
    base.int32(33, p.main_chapter);      // MainChapter
    base.int32(34, p.main_section);      // MainSection: 0 sends MainHallFSM into the newbie battle video
    base.int32(8, p.show_hero);          // Show: 主界面神格展示英雄 (个人信息页/主大厅立绘)
    {
        Writer icon; // IconInfoProto: 1 iconType, 2 iconID, 3 ornamentID, 4 repeated PictureID
        icon.int32(1, 1);
        icon.int32(2, p.icon_id);
        icon.int32(3, p.ornament_id);
        GameTable picture;
        const bool have_picture = tables && picture.load(*tables, "Picture");
        std::int32_t idx = 0;
        const auto add_unlock = [&](std::int32_t pid) {
            if (pid <= 0) return;
            if (have_picture) {
                const auto row = picture.row(static_cast<std::uint64_t>(pid));
                if (!row || picture.int_field(*row, 5).value_or(0) != 1) return; // IsUse!=1
            }
            Writer entry; // ContainerIntIntProto: 1 idx, 2 val
            entry.int32(1, idx++);
            entry.int32(2, pid);
            icon.message(4, entry.data());
        };
        add_unlock(1000001);
        for (const auto& hero : account.heroes) {
            for (const auto pid : icon_unlock_ids(hero, tables, &account)) add_unlock(pid);
        }
        base.message(9, icon.data());
    }
    Writer mobility;
    mobility.int32(1, p.power);          // MobilityProto.Power
    Writer player;
    player.message(1, base.data());
    player.message(2, mobility.data());  // PlayerDataProto.Mobility

    {
        Writer daily;
        daily.int64(7, p.touch_total);
        player.message(3, daily.data());
    }

    Writer module_status; // ModuleStatusProto: 1 LightYard, 2 Task, 3 GrowthBase, 4 Draw, 5 Shop, 6 Club
    module_status.int32(3, 1);           // GrowthBaseStatus: 白夜行星已开启
    player.message(4, module_status.data());
    {
        Writer medal;
        Writer pack;
        for (const auto& [id, obtained] : p.medal_pack) {
            Writer entry;
            entry.int32(1, id);
            entry.int64(2, obtained);
            pack.message(1, entry.data());
        }
        medal.message(1, pack.data());
        Writer show;
        for (const auto& [slot, id] : p.medal_show) {
            if (slot < 0 || slot > 2 || id <= 0) continue;
            Writer entry;
            entry.int32(1, slot);
            entry.int32(2, id);
            show.message(1, entry.data());
        }
        medal.message(2, show.data());
        player.message(7, medal.data());
    }

    if (!p.relic_pack.empty()) {
        std::int32_t slot = 0;
        for (const auto relic : p.relic_pack) {
            Writer entry;
            entry.int32(1, slot);
            entry.int32(2, relic);
            player.message(11, entry.data());
            ++slot;
        }
    }
    if (tables) {
        std::int32_t slot = 0;
        Writer star;
        for (const auto id : unlocked_stars(account, *tables)) {
            Writer entry;
            entry.int32(1, slot++);
            entry.int32(2, id);
            star.message(1, entry.data());
        }
        for (const auto& [id, level] : p.star_skills) {
            if (level <= 0) continue;
            Writer entry;
            entry.int32(1, id);
            entry.int32(2, level);
            star.message(2, entry.data());
        }
        if (slot > 0 || !p.star_skills.empty()) player.message(8, star.data());
    }
    if (!p.equip_plans.empty() || !p.cleared_equip_plans.empty()) {
        Writer plans;
        for (const auto& plan : p.equip_plans) {
            Writer body;
            body.string(1, plan.name);
            for (const auto& [slot, equip_id] : plan.positions) {
                Writer pos;
                pos.int32(1, slot);
                pos.int32(2, equip_id);
                body.message(3, pos.data());
            }
            Writer entry;
            entry.int32(1, plan.id);
            entry.message(2, body.data());
            plans.message(1, entry.data());
        }
        for (const auto id : p.cleared_equip_plans) {
            if (std::ranges::contains(p.equip_plans, id, &AccountEquipPlan::id)) continue;
            Writer entry;
            entry.int32(1, id);
            plans.message(1, entry.data());
        }
        player.message(9, plans.data());
    }
    for (const auto& hero : account.heroes) {
        const auto level = hero.favor_level > 0 ? hero.favor_level : 1;
        Writer favor;
        favor.int32(1, level);
        favor.int32(2, hero.favor_exp);
        favor.int32(7, 0); // gifts_times 默认为 0
        Writer entry;
        entry.int32(1, hero.id);
        entry.message(2, favor.data());
        player.message(15, entry.data());
    }
    return player.data();
}

namespace {
void ints(Writer& writer, std::uint32_t field, const std::vector<std::int32_t>& values) {
    for (const auto value : values) writer.int32(field, value);
}
} // namespace

std::vector<std::uint8_t> ProtocolBuilders::query_activity(const TableBlob& tables, std::int32_t) {
    Writer reply;
    reply.int32(1, static_cast<std::int32_t>(server::LogicCode::Ok));
    GameTable table;
    if (!table.load(tables, "ActivityReal")) return reply.data();
    for (const auto& row : table.rows()) {
        Writer act;
        act.int32(1, table.int_field(row, 1).value_or(0));
        act.int32(2, 0); // ActivityRealState.CLOSE
        act.int32(3, table.int_field(row, 2).value_or(0));
        act.int32(4, table.int_field(row, 3).value_or(0));
        act.int32(7, table.int_field(row, 7).value_or(0));
        act.int32(8, table.int_field(row, 6).value_or(0));
        act.int32(9, table.int_field(row, 4).value_or(0));
        act.int32(10, table.int_field(row, 5).value_or(0));
        ints(act, 11, table.ints_field(row, 12));
        ints(act, 12, table.ints_field(row, 13));
        ints(act, 13, table.ints_field(row, 14));
        ints(act, 14, table.ints_field(row, 15));
        act.string(15, table.string_field(row, 17).value_or(""));
        ints(act, 16, table.ints_field(row, 16));
        act.string(18, table.string_field(row, 20).value_or(""));
        act.int32(19, table.int_field(row, 8).value_or(0));
        ints(act, 20, table.ints_field(row, 9));
        reply.message(2, act.data());
    }
    return reply.data();
}

std::vector<std::uint8_t> ProtocolBuilders::fight_data(const TableBlob* tables, const Account& account,
                                                       const std::vector<std::int32_t>& hero_ids, std::int32_t mission) {
    struct Base {
        std::int32_t attrib_id;
        std::uint32_t player_attrib_field;
        std::int64_t fallback;
    };
    static constexpr Base kBase[] = {
        {100, 18, 60},   // E_ATK     <- damage
        {102, 19, 40},   // E_DEF     <- defense
        {104, 15, 600},  // E_HP      <- hPMax
        {106, 17, 3000}, // E_SP      <- sPMax
        {112, 25, 50},   // E_CRI     <- critical
        {113, 26, 0},    // E_CRI_Dmg <- criticalDamage
        {136, 20, 550},  // E_MV      <- moveSpeed
    };
    GameTable attrib;
    const bool has_attrib = tables != nullptr && attrib.load(*tables, "PlayerAttrib");

    // Tags from Serialize in IDA. FightData has no field 1.
    Writer fight;
    for (const auto id : hero_ids) {
        const auto owned = std::ranges::find(account.heroes, id, &AccountHero::id);
        const auto row = has_attrib ? attrib.row(static_cast<std::uint64_t>(id)) : std::nullopt;
        const auto stat = [&](const Base& base) -> std::int64_t {
            return row ? attrib.int_field(*row, base.player_attrib_field).value_or(base.fallback) : base.fallback;
        };
        Writer hero; // FightHero: 1 id, 2 state, 3 level, 4 star, 5 heroGodEquip,
                     // 9 attrAdd, 10 battleSkinId, 12 heroAttrCount
        hero.int32(1, id);
        hero.int32(2, 1);
        hero.int32(3, owned != account.heroes.end() ? owned->level : 1);
        hero.int32(4, owned != account.heroes.end() ? owned->star : 1);
        hero.message(5, {}); // ConvertHeroGodEquip throws on null (list fields are null-safe)
        for (const auto& base : kBase) {
            Writer add; // HeroAttrAdd: 1 attrId, 2 attrValue
            add.int32(1, base.attrib_id);
            add.int64(2, stat(base));
            hero.message(9, add.data());
        }
        // 战斗形象
        if (owned != account.heroes.end() && owned->battle_skin > 0) hero.int32(10, owned->battle_skin);
        Writer count; // HeroAttrCount: 1 atk, 2 def, 3 hp, 4 sp (display totals)
        count.int64(1, stat(kBase[0]));
        count.int64(2, stat(kBase[1]));
        count.int64(3, stat(kBase[2]));
        count.int64(4, stat(kBase[3]));
        hero.message(12, count.data());
        fight.message(2, hero.data()); // fightHeros
    }
    fight.int32(3, mission); 
    fight.message(4, {});
    return fight.data();
}

std::vector<std::uint8_t> ProtocolBuilders::query_mission(const TableBlob* tables, const Account& account) {
    Writer reply;
    const auto highest_cleared = [&](std::string_view table_name, std::uint32_t list_field, std::int32_t type) {
        GameTable table;
        if (!tables || !table.load(*tables, table_name)) return;
        Writer group;
        group.int32(1, type);
        for (const auto& row : table.rows()) {
            const auto levels = table.ints_field(row, list_field);
            const auto top = std::ranges::find_last_if(levels, [&](std::int32_t section) {
                return std::ranges::contains(account.player.cleared_main, section);
            });
            if (top.empty()) continue;
            Writer entry;
            entry.int32(1, table.int_field(row, 1).value_or(0));
            entry.int32(2, top.front());
            group.message(2, entry.data());
        }
        reply.message(1, group.data());
    };
    highest_cleared("ChapterInfo", 6, 2);
    highest_cleared("DailyDungeon", 7, 3);
    for (const auto id : account.player.cleared_main) reply.int32(2, id);
    return reply.data();
}

std::vector<std::uint8_t> ProtocolBuilders::card_pool(const TableBlob& tables, const Account& account,
                                                     std::int32_t server_time) {
    Writer reply;
    reply.int32(1, static_cast<std::int32_t>(server::LogicCode::Ok));
    GameTable draw;
    if (!draw.load(tables, "DrawParam")) return reply.data();
    for (const auto id : kOpenPools) {
        const auto row = draw.row(static_cast<std::uint64_t>(id));
        if (!row) continue;
        const auto it = account.player.draw_pools.find(id);
        const auto state = it == account.player.draw_pools.end() ? AccountDrawPool{} : it->second;
        Writer pool; 
        pool.int32(1, id);
        pool.int32(2, client_time(server_time - 86400));
        pool.int32(3, client_time(server_time + 365 * 86400));
        pool.int32(4, draw.int_field(*row, 18).value_or(0));
        pool.int32(5, state.one);
        pool.int32(6, state.ten);
        pool.int32(7, state.since_top);
        pool.int32(8, state.one + 10 * state.ten);
        reply.message(2, pool.data());
    }
    return reply.data();
}

std::int32_t ProtocolBuilders::inside_battle_price(const TableBlob& tables, std::int32_t item_id) {
    GameTable item;
    if (!item.load(tables, "Item")) return 0;
    const auto row = item.row(static_cast<std::uint64_t>(item_id));
    return row ? maze_price(item, *row) : 0;
}

std::vector<std::uint8_t> ProtocolBuilders::inside_battle_shop(const TableBlob& tables, std::int32_t shop_id,
                                                               std::int32_t floor, std::int32_t section) {
    Writer reply;
    reply.int32(1, static_cast<std::int32_t>(server::LogicCode::Ok));
    reply.int32(2, shop_id);
    reply.int32(3, floor);
    GameTable item;
    if (!item.load(tables, "Item")) return reply.data();
    std::vector<std::pair<std::int32_t, std::int32_t>> relics, consumables; // id, price
    for (const auto& row : item.rows())
        if (const auto price = maze_price(item, row))
            (item.int_field(row, 6) == 4 ? relics : consumables).emplace_back(static_cast<std::int32_t>(row.key), price);
    std::mt19937 rng(static_cast<std::uint32_t>(section) * 1000003u + static_cast<std::uint32_t>(shop_id) * 131u +
                     static_cast<std::uint32_t>(floor));
    std::ranges::shuffle(relics, rng);
    std::ranges::shuffle(consumables, rng);
    const auto put = [&](const auto& goods, std::size_t n) {
        for (const auto& [id, price] : goods | std::views::take(n)) {
            Writer goods_item; // ShopItem: 1 itemId, 2 Num, 3 Price, 4 Discount (<100 shows a badge), 5 MoneyType
            goods_item.int32(1, id);
            goods_item.int32(2, 1);
            goods_item.int32(3, price);
            goods_item.int32(4, 100);
            goods_item.int32(5, 903);
            reply.message(4, goods_item.data());
        }
    };
    put(relics, 4);
    put(consumables, 2);
    return reply.data();
}

std::vector<std::uint8_t> ProtocolBuilders::reconnect(const Account& account, std::int32_t server_time) {
    Writer writer;
    writer.int32(1, static_cast<std::int32_t>(server::LogicCode::Ok));
    writer.int64(2, account.id);
    writer.int32(4, client_time(server_time));
    return writer.data();
}

} // namespace x2::offline
