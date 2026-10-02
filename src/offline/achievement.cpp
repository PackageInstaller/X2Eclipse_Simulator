#include "offline/achievement.hpp"

#include <algorithm>
#include <cstdlib>
#include <ranges>

#include "offline/game_table.hpp"
#include "offline/mission.hpp"
#include "proto/protobuf.hpp"

namespace x2::offline {

namespace {

std::int32_t count_heroes_at_level(const Account& account, std::int32_t level) {
    std::int32_t n = 0;
    for (const auto& h : account.heroes) {
        if (h.level >= level) ++n;
    }
    return n;
}

// PlayerStage.BigStarNum，大小星
std::int32_t hero_big_star(const TableBlob* tables, std::int32_t stage) {
    if (!tables || stage <= 0) return 0;
    GameTable ps;
    if (!ps.load(*tables, "PlayerStage")) return stage;
    const auto row = ps.row(static_cast<std::uint64_t>(stage));
    return row ? ps.int_field(*row, 8).value_or(0) : 0;
}

std::int32_t count_heroes_at_star(const Account& account, const TableBlob* tables, std::int32_t star) {
    std::int32_t n = 0;
    for (const auto& h : account.heroes) {
        if (hero_big_star(tables, h.star) >= star) ++n;
    }
    return n;
}

std::int32_t count_heroes_at_favor(const Account& account, std::int32_t level) {
    std::int32_t n = 0;
    for (const auto& h : account.heroes) {
        if ((h.favor_level > 0 ? h.favor_level : 1) >= level) ++n;
    }
    return n;
}

std::int32_t count_equips_at_level(const Account& account, std::int32_t level) {
    std::int32_t n = 0;
    for (const auto& e : account.player.equips) {
        if (e.level >= level) ++n;
    }
    return n;
}

std::int32_t count_equips_at_star(const Account& account, std::int32_t star) {
    std::int32_t n = 0;
    for (const auto& e : account.player.equips) {
        if (e.star >= star) ++n;
    }
    return n;
}

std::int32_t chapters_cleared(const Account& account) {
    (void)account;
    return 0;
}


bool chapter_cleared(const Account& account, const TableBlob* tables, std::int32_t chapter_number) {
    GameTable info;
    if (!tables || !info.load(*tables, "ChapterInfo")) return false;
    for (const auto& row : info.rows()) {
        if (info.int_field(row, 5).value_or(0) != chapter_number) continue;
        const auto stages = info.ints_field(row, 2);
        return !stages.empty() && std::ranges::all_of(stages, [&](std::int32_t s) {
                   return std::ranges::contains(account.player.cleared_main, s);
               });
    }
    return false;
}

std::int32_t college_building_level(const Account& account, std::int32_t building_id) {
    if (const auto it = account.player.college.find(building_id); it != account.player.college.end()) {
        return it->second.first;
    }
    return 1; // CollegeBuilding.InitialLevel
}

std::int32_t wonder_level(const Account& account, std::int32_t building_id) {
    if (const auto it = account.player.college.find(building_id); it != account.player.college.end()) {
        return it->second.first;
    }
    return 0; // 未建造
}

std::int32_t hero_favor_level(const Account& account, std::int32_t hero_id) {
    for (const auto& h : account.heroes) {
        if (h.id == hero_id) return h.favor_level > 0 ? h.favor_level : 1;
    }
    return 0;
}

} // namespace

std::int32_t achv_progress(std::int32_t achievement_id, const Account& account,
                           const TableBlob* tables) {
    if (!tables) return 0;
    GameTable achievement;
    if (!achievement.load(*tables, "Achievement")) return 0;
    const auto row = achievement.row(static_cast<std::uint64_t>(achievement_id));
    if (!row) return 0;
    const auto cond_id = achievement.int_field(*row, 5).value_or(0);
    if (cond_id <= 0) return 0;


    if (cond_id == 621000) return account.player.level;                    // 解神者等级
    if (cond_id >= 621030 && cond_id <= 621034) return static_cast<std::int32_t>(account.heroes.size());
    if ((cond_id >= 621050 && cond_id <= 621067)) {
        // 拥有 N 名 X 级神格: 条件 id 尾数映射等级档位
        static constexpr std::int32_t kLevels[] = {10, 15, 20, 25, 30, 30, 35, 40, 40, 45, 50, 50, 55, 60, 60, 65, 70, 70};
        const auto idx = cond_id - 621050;
        if (idx < static_cast<std::int32_t>(std::size(kLevels))) return count_heroes_at_level(account, kLevels[idx]);
    }
    if (cond_id >= 621080 && cond_id <= 621097) {
        static constexpr std::int32_t kStars[] = {3, 3, 3, 4, 4, 4, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6};
        const auto idx = cond_id - 621080;
        if (idx < static_cast<std::int32_t>(std::size(kStars))) return count_heroes_at_star(account, tables, kStars[idx]);
    }
    if (cond_id >= 621110 && cond_id <= 621112) return static_cast<std::int32_t>(account.player.equips.size());
    if (cond_id >= 621120 && cond_id <= 621128) {
        static constexpr std::int32_t kEqLevels[] = {6, 6, 6, 12, 12, 12, 15, 15, 15};
        const auto idx = cond_id - 621120;
        if (idx < static_cast<std::int32_t>(std::size(kEqLevels))) return count_equips_at_level(account, kEqLevels[idx]);
    }
    if (cond_id >= 621140 && cond_id <= 621143) {
        static constexpr std::int32_t kEqStars[] = {4, 4, 4, 4};
        return count_equips_at_star(account, kEqStars[cond_id - 621140]);
    }
    // 玩法 (622xxx)
    if (cond_id == 622000) return account.login_count > 0 ? account.login_count : 1; // 累计登录天数 (近似)
    if (cond_id >= 622050 && cond_id <= 622059) {
        return chapter_cleared(account, tables, cond_id - 622050 + 2) ? 1 : 0;
    }
    if (cond_id >= 622070 && cond_id <= 622079) return 0; // 现世复刻计时 (未实现)
    if (cond_id == 622120) return college_building_level(account, 701); // 白夜大厅
    if (cond_id == 622140) return college_building_level(account, 703); // 启明之塔(收藏)
    if (cond_id == 622160) return college_building_level(account, 705); // 时空回廊(探索)
    if (cond_id == 622180) return college_building_level(account, 704); // 光之密室(训练)
    if (cond_id == 622200) return college_building_level(account, 702); // 时空之门(能量)
    if (cond_id == 622220) return college_building_level(account, 706); // 镜面立方(仓库)
    if (cond_id == 622240) return college_building_level(account, 707); // 逆位螺旋(熔炉)
    if (cond_id == 622300) return 0; // 炼金收获金币 (未实现计数)
    if (cond_id == 622310) return 0; // 派遣探险次数 (未实现)
    if (cond_id == 622320) return 0; // 训练次数 (未实现)
    if (cond_id == 622330) return 0; // 炼金交易 (未实现)
    if (cond_id == 622340) return 0; // 炼金任务 (未实现)
    if (cond_id >= 622400 && cond_id <= 622460) return wonder_level(account, 721 + (cond_id - 622400) / 10);
    if (cond_id == 622600) return 0; // 祈神次数 (未实现)
    if (cond_id == 622700) return static_cast<std::int32_t>(account.player.owned_skins.size()); // 白夜补给购物
    if (cond_id >= 622710 && cond_id <= 622730) return 0; // 击败怪物 (未实现计数)
    if (cond_id >= 622900 && cond_id <= 622909) return 0; // 血月复刻 (未实现)
    // 社交 (62 0001-620039)
    if (cond_id == 620001) return 0; // 世界频道发言 (未实现计数)
    if (cond_id == 620010) return 0; // 累计友情点 (未实现计数)
    if (cond_id >= 620030 && cond_id <= 620039) return 0; // 好友数 (未实现)
    // 默契度 (620250-620700)
    if (cond_id >= 620250 && cond_id <= 620257) {
        static constexpr std::int32_t kFavor[] = {5, 5, 5, 10, 10, 10, 10, 10};
        return count_heroes_at_favor(account, kFavor[cond_id - 620250]);
    }
    if (cond_id == 620270) return account.player.stat_talk_given; // 与神格交谈
    if (cond_id == 620280) return 0; // 心愿任务 (未实现)
    if (cond_id == 620290) return account.player.stat_gift_given; // 赠送礼物
    if (cond_id >= 620310 && cond_id <= 620700) {
        // 指定英雄默契度: 620310 + (英雄序号 * 10); 英雄序号按表顺序 (1003 起)
        const auto idx = (cond_id - 620310) / 10;
        static constexpr std::int32_t kHeroIds[] = {
            1003, 1004, 1005, 1006, 1007, 1008, 1009, 1010, 1011, 1012, 1013, 1014,
            1015, 1016, 1017, 1018, 1019, 1020, 1021, 1022, 1023, 1024, 1025, 1026,
            1027, 1028, 1029, 1030, 1031, 1032, 1033, 1034, 1035, 1036, 1037, 1038,
            1039, 1040, 1041};
        if (idx < static_cast<std::int32_t>(std::size(kHeroIds))) return hero_favor_level(account, kHeroIds[idx]);
    }
    return 0;
}

std::int32_t achv_target(std::int32_t achievement_id, const TableBlob* tables) {
    if (!tables) return 0;
    GameTable achievement;
    if (!achievement.load(*tables, "Achievement")) return 0;
    const auto row = achievement.row(static_cast<std::uint64_t>(achievement_id));
    if (!row) return 0;
    const auto cond_id = achievement.int_field(*row, 5).value_or(0);
    const auto cond_type = achievement.int_field(*row, 4).value_or(0);
    if (cond_type == 1) { // Line: 目标 = 最后一阶
        GameTable line;
        if (line.load(*tables, "AchievementConditionLine")) {
            if (const auto lrow = line.row(static_cast<std::uint64_t>(cond_id))) {
                const auto stages = line.ints_field(*lrow, 2);
                if (!stages.empty()) return stages.back();
            }
        }
        return 0;
    }
    GameTable cond;
    if (cond.load(*tables, "AchievementCondition")) {
        if (const auto crow = cond.row(static_cast<std::uint64_t>(cond_id))) {
            return cond.int_field(*crow, 2).value_or(0);
        }
    }
    return 0;
}

std::int32_t achv_stage(std::int32_t achievement_id, const Account& account, const TableBlob* tables) {
    if (!tables) return 0;
    GameTable achievement;
    if (!achievement.load(*tables, "Achievement")) return 0;
    const auto row = achievement.row(static_cast<std::uint64_t>(achievement_id));
    if (!row) return 0;
    const auto cond_id = achievement.int_field(*row, 5).value_or(0);
    const auto cond_type = achievement.int_field(*row, 4).value_or(0);
    const auto progress = achv_progress(achievement_id, account, tables);
    if (cond_type == 1) {
        GameTable line;
        if (line.load(*tables, "AchievementConditionLine")) {
            if (const auto lrow = line.row(static_cast<std::uint64_t>(cond_id))) {
                const auto stages = line.ints_field(*lrow, 2);
                std::int32_t stage = 0;
                for (const auto s : stages) {
                    if (progress >= s) ++stage;
                    else break;
                }
                return stage;
            }
        }
        return 0;
    }
    return progress >= achv_target(achievement_id, tables) ? 1 : 0;
}

std::int32_t achv_total_points(const Account& account, const TableBlob* tables) {
    if (!tables) return 0;
    GameTable achievement;
    if (!achievement.load(*tables, "Achievement")) return 0;
    std::int32_t points = 0;
    for (const auto& row : achievement.rows()) {
        if (achievement.int_field(row, 10).value_or(1) != 1) continue; // IsUse
        const auto id = static_cast<std::int32_t>(row.key);
        // 点数只算已领取的
        if (std::ranges::contains(account.player.achv_claimed, id)) ++points;
    }
    return points;
}

std::vector<std::int32_t> achv_point_thresholds(const TableBlob* tables) {
    // TaskControl(id=5).CompleteAchievementNumber (f9)
    static const std::vector<std::int32_t> kFallback{10, 20, 30, 50, 80, 120, 160, 200, 250, 300, 350, 400, 450, 500, 550};
    if (!tables) return kFallback;
    GameTable control;
    if (!control.load(*tables, "TaskControl")) return kFallback;
    const auto row = control.row(5);
    if (!row) return kFallback;
    const auto numbers = control.ints_field(*row, 9); // f9 CompleteAchievementNumber
    return numbers.empty() ? kFallback : numbers;
}

// 点数里程碑奖励: TaskControl(id=5).CompleteAchievementNumberGiftGroup (f10)
std::int32_t achv_point_gift_group(const TableBlob* tables, std::int32_t point_id) {
    if (!tables) return 0;
    GameTable control;
    if (!control.load(*tables, "TaskControl")) return 0;
    const auto row = control.row(5);
    if (!row) return 0;
    const auto gifts = control.ints_field(*row, 10); // f10 gift groups
    if (point_id < 0 || point_id >= static_cast<std::int32_t>(gifts.size())) return 0;
    return gifts[static_cast<std::size_t>(point_id)];
}

AchvClaimResult claim_achievement(Account& account, const TableBlob* tables,
                                  std::int32_t achievement_id) {
    AchvClaimResult res;
    res.achv_id = achievement_id;
    if (!tables || achievement_id <= 0) {
        res.code = 13;
        return res;
    }
    if (std::ranges::contains(account.player.achv_claimed, achievement_id)) {
        res.code = 80; // E_REWARD_HAS_PICK: 收到 80 清对应红点
        return res;
    }
    const auto progress = achv_progress(achievement_id, account, tables);
    if (progress < achv_target(achievement_id, tables)) {
        res.code = 79; // E_TASK_NOT_FINISH
        return res;
    }
    GameTable achievement, gift;
    if (!achievement.load(*tables, "Achievement") || !gift.load(*tables, "Gift")) {
        res.code = 13;
        return res;
    }
    const auto row = achievement.row(static_cast<std::uint64_t>(achievement_id));
    if (!row) {
        res.code = 13;
        return res;
    }
    for (const auto group : achievement.ints_field(*row, 9)) {
        std::vector<AccountItem> rewards;
        expand_gift(gift, group, rewards);
        for (const auto& reward : rewards) {
            if (reward.id <= 0 || reward.num <= 0) continue;
            res.shown.emplace_back(reward.id, reward.num);
            std::optional<std::int32_t> enu = item_currency_enu(tables, reward.id);
            if (enu) {
                res.player = true;
                auto& p = account.player;
                switch (*enu) {
                    case E_Gold: p.gold = clamp_add(p.gold, reward.num); break;
                    case E_Money: p.crystal = clamp_add(p.crystal, reward.num); break;
                    case E_JewelChip: p.jewel_chip = clamp_add(p.jewel_chip, reward.num); break;
                    case E_HeroExp: p.hero_exp = clamp_add(p.hero_exp, reward.num); break;
                    case E_SkillPoint: p.skill_point = clamp_add(p.skill_point, reward.num); break;
                    case E_PowerOfLight: p.power_of_light = clamp_add(p.power_of_light, reward.num); break;
                    default:
                        if (auto* field = wallet(p, *enu)) *field = clamp_add(*field, reward.num);
                        break;
                }
                continue;
            }
            auto it = std::ranges::find(account.items, reward.id, &AccountItem::id);
            if (it == account.items.end()) it = account.items.insert(account.items.end(), AccountItem{.id = reward.id});
            it->num = clamp_add(it->num, reward.num);
            res.bag.push_back(*it);
        }
    }
    account.player.achv_claimed.push_back(achievement_id);
    res.ok = true;
    res.status = 2;
    return res;
}

AchvPointClaimResult claim_achievement_point(Account& account, const TableBlob* tables,
                                             std::int32_t point_id) {
    AchvPointClaimResult res;
    res.point_id = point_id;
    const auto thresholds = achv_point_thresholds(tables);
    if (point_id < 0 || point_id >= static_cast<std::int32_t>(thresholds.size())) {
        res.code = 13;
        return res;
    }
    if (std::ranges::contains(account.player.achv_point_claimed, point_id)) {
        res.code = 80; // E_REWARD_HAS_PICK: 清红点
        return res;
    }
    const auto points = achv_total_points(account, tables);
    if (points < thresholds[static_cast<std::size_t>(point_id)]) {
        res.code = 79;
        return res;
    }
    if (const auto group = achv_point_gift_group(tables, point_id); group > 0) {
        GameTable gift;
        if (gift.load(*tables, "Gift")) {
            std::vector<AccountItem> rewards;
            expand_gift(gift, group, rewards);
            for (const auto& reward : rewards) {
                if (reward.id <= 0 || reward.num <= 0) continue;
                res.shown.emplace_back(reward.id, reward.num);
                if (const auto enu = item_currency_enu(tables, reward.id)) {
                    res.player = true;
                    auto& p = account.player;
                    if (auto* field = wallet(p, *enu)) *field = clamp_add(*field, reward.num);
                    continue;
                }
                auto it = std::ranges::find(account.items, reward.id, &AccountItem::id);
                if (it == account.items.end()) it = account.items.insert(account.items.end(), AccountItem{.id = reward.id});
                it->num = clamp_add(it->num, reward.num);
                res.bag.push_back(*it);
            }
        }
    }
    account.player.achv_point_claimed.push_back(point_id);
    res.ok = true;
    res.status = 2;
    return res;
}

} // namespace x2::offline
