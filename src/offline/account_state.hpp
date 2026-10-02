#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace x2::offline {


struct AccountItem {
    std::int32_t id{};
    std::int32_t num{};
    bool locked{};
    std::int32_t day_get{};
    std::int32_t show{};
};


struct AccountEquip {
    std::int32_t id{};
    std::int32_t type_id{};
    std::int32_t level{1};
    std::int32_t star{1};
    std::map<std::int32_t, std::int32_t> attrs;
};

struct AccountEquipPlan {
    std::int32_t id{};
    std::string name;
    std::map<std::int32_t, std::int32_t> positions;
};

// 魂器（专武）状态持久化: id (武器ID), level (进度百分比 0..100), star (0未解锁, 1..6星), jewels (孔位 0..4 -> 宝石道具ID)
struct AccountArtifact {
    std::int32_t id{};
    std::int32_t level{};
    std::int32_t star{};
    // 觉醒属性槽 (BattlepassArtifact): slot -> {attrType, attrVal}。
    // 专武进度等级 L = level/8 + 1; 槽 1 随激活解锁, 槽 2/3/4/5 于 L2/5/8/11 解锁。
    std::map<std::int32_t, std::pair<std::int32_t, std::int32_t>> properties;
    std::map<std::int32_t, std::int32_t> jewels;
};

struct AccountHero {
    std::int32_t id{};
    std::int32_t state{};
    std::int32_t level{};
    std::int32_t star{};
    std::int64_t exp{};
    std::int64_t get_time{};
    std::string name;
    bool god_shed{}; // 神降第一段: SkillBase.pos E_GodSpell is on the hero
    std::vector<std::int32_t> god_slots; // 神降第二段: GodHole hole numbers
    std::int32_t favor_level{}; // 0 = unset, client uses FavorabilityHero.InitialLevel
    std::int32_t favor_exp{};
    std::map<std::int32_t, std::int32_t> worn; // HeroData.equips: slot 1-6 -> equip instance id
    AccountArtifact artifact; // 魂器专武状态
    std::map<std::int32_t, std::int32_t> skills; // 技能等级: skill id -> level
    std::map<std::int32_t, std::int32_t> archives; // 档案状态: file id -> status (0: Lock, 1: CanUnLock, 2: Unlocked)
    std::map<std::int32_t, std::int32_t> fetters; // 联结羁绊等级: positionId (posId) -> level
    std::vector<std::int32_t> dubbings; // 已解锁语音: Dubbing.DubbingId (L2C_SaveHeroDubbing 下发)
    std::int32_t battle_skin{}; // 战斗外显皮肤外观ID (0 = 默认)
    std::int32_t outer_skin{};  // 主界面外显皮肤外观ID (0 = 默认)
};

// 白夜行星
struct CollegeBuild {
    std::int32_t building_id{}; // 0 = idle
    std::int64_t end_unix{};    // completion wall-clock (unix seconds)
    std::int32_t duration_sec{};// original build duration, for the client bar
};

struct AccountMail {
    std::int64_t id{};
    std::int32_t item_id{};
    std::int32_t num{};
    std::int64_t time{};
    std::int32_t state{}; // MailState: 0 new, 2 received
};


struct AccountDrawRecord {
    std::int32_t item_id{};
    std::int32_t num{};
    bool trans{}; // owned hero turned into chips
    std::int64_t time{}; // unix seconds
};

struct AccountDrawPool {
    std::int32_t one{};
    std::int32_t ten{};
    std::int32_t since_security{};
    std::int32_t since_top{};
    std::vector<AccountDrawRecord> history;
};

struct AccountPlayer {
    std::int32_t level{1};
    std::int32_t exp{};
    std::int32_t gold{};
    std::int32_t crystal{};
    std::int32_t jewel_chip{};
    std::int32_t equip_exp{};
    std::int32_t power{60};
    // BaseInfo.MainChapter/MainSection. MainSection 0 = fresh account: the
    // main hall starts the newbie battle video instead of the hall.
    std::int32_t hero_exp{};          // BaseInfo.HeroExp: currency E_HeroExp (item 1237907)
    std::int32_t skill_point{};       // BaseInfo.StarSkillPoint: currency E_SkillPoint (item 1237916)
    std::int32_t power_of_light{};    // BaseInfo.PowerOfLight: E_PowerOfLight 光能 (item 1237913)
    std::int32_t vow_coin{};          // BaseInfo.VowOfCoin: E_VowOfCoin 许愿币 (item 1237914)
    std::int32_t wish_crystal{};      // BaseInfo.WishCrystal: E_Crystal 许愿水晶 (item 1237915)
    std::int32_t skin_coupon{};
    std::int32_t friend_coin{}; // BaseInfo.FriendCoin 友情点 (CurrencyEnu 17)       // BaseInfo.SkinCoupon (Tag 47): currency E_SkinTicket (item 1237923, CurrencyType 923)
    std::map<std::int32_t, AccountDrawPool> draw_pools; // DrawParam id
    std::vector<std::int32_t> course_levels; // 特别课程 (GameActivity 100002) levels whose free gift was taken
    // Highest role level whose RoleExp.giftID was already granted. 0 = not
    // recorded yet (older saves); hydrate grants the gap once.
    std::int32_t rewarded_level{};
    std::vector<std::int32_t> collection_awards; // Collection.collectionID already claimed
    std::int32_t daily_activity{};               // BaseInfo.DailyActivity, fills the daily boxes
    std::vector<std::int32_t> daily_tasks;       // DailyTask ids already claimed
    std::vector<std::int32_t> daily_boxes;       // TaskControl thresholds already picked
    std::map<std::int32_t, std::int32_t> daily_progress; // TaskConditionID -> count, daily tasks
    std::int32_t task_day{};                     // game day the daily fields above belong to
    std::vector<std::int32_t> challenge_tasks;   // ChallengeTask ids already claimed
    std::vector<std::int32_t> challenge_boxes;   // Challenge phase ids whose chest was picked
    std::map<std::int32_t, std::int32_t> star_skills; // StarMap.StarSkill: skill id -> level
    std::string nickname{"offline"};
    std::int32_t show_hero{1003};  // BaseInfo.Show: 主界面神格展示的英雄 id
    std::int32_t icon_id{1270301}; // BaseInfo.IconInfo.iconID: 当前头像 (Picture 表行)
    std::int32_t ornament_id{};    // BaseInfo.IconInfo.ornamentID
    std::int32_t touch_day{};      // 触摸互动计数所属 game day (与 task_day 同口径)
    std::int32_t touch_total{};    // 当日触摸总次数 (GlobalParamString FavorabilityDailyLimit=15)
    std::map<std::int32_t, std::int32_t> touch_hero; // 当日每英雄触摸次数 (FavorabilityHeroDailyLimit=5)
    // 成就系统: achv_claimed = 已领奖励的 Achievement id; achv_point_claimed = 已领的
    // 点数里程碑阈值索引 (kAchvPointThresholds 下标); 计数器供成就条件使用。
    std::vector<std::int32_t> achv_claimed;
    std::vector<std::int32_t> achv_point_claimed;
    std::int32_t stat_gift_given{}; // 累计赠送神格礼物次数 (成就 620290)
    std::int32_t stat_talk_given{}; // 累计与神格交谈次数 (成就 620270)
    // 徽章系统 (Medal 表 126xxxx): medal_pack = medalId -> 获得时间 unix 秒
    // (客户端 CheckMedalUnlock 要求 >=1 且未过期); medal_show = 主页展示槽
    // 0..2 -> medalId (0 = 空), C2L_MedalOpt(411) 持久化。
    std::map<std::int32_t, std::int64_t> medal_pack;
    std::map<std::int32_t, std::int32_t> medal_show;
    // 终端-时光 (NpcBlog 484/486/488): 客户端 MergeBox 按 groupID 整组替换, 回包必须
    // 带该英雄全部帖子的最新状态, 所以赞/回复要在服务器持久化。
    // npc_blog_likes = BlogID -> likeTime (kFavorTimeBase 偏移值, 0 语义未用);
    // npc_blog_replies = BlogID -> 玩家回复记录 (replyID 必须在 FavorabilityBlog.
    // ReplyContent 列表里, 客户端 IndexOf 后查 SecondReplyHero/Content 渲染回应)。
    std::map<std::int32_t, std::int32_t> npc_blog_likes;
    struct NpcBlogReply {
        std::int32_t reply_id{};
        std::int32_t time_offset{};
        std::int32_t chat_group_id{};
    };
    std::map<std::int32_t, std::vector<NpcBlogReply>> npc_blog_replies;
    // 神迹收集 (Item ItemType=4 遗物 1004001-1004412): 战斗中拾取 → 持久化,
    // PlayerData.f11 RelicPack 下发。训练场/复刻选神迹的激活判定
    // (ChapterModule.CheckHasRelic) 读它, 缺失 = "该神迹尚未激活"。
    std::vector<std::int32_t> relic_pack;
    std::int32_t main_chapter{1};
    std::int32_t main_section{0};
    std::vector<std::int32_t> cleared_main; // L2C_QueryMission.mainMission
    // BaseInfo.QuestIDs: TitoGuide group -> 0 pending / 1 finished. Empty =
    // not seeded yet (the client ends the guide system on empty QuestIDs).
    // A group is inserted when its FunctionOpen.openLevel is reached. The
    // client starts a pending group only when this map changes.
    std::map<std::int32_t, std::int32_t> guides;
    std::vector<AccountEquip> equips;
    std::vector<AccountEquipPlan> equip_plans;
    // EquipPlan list index == plan id. Merge only clears a slot when that
    // index is sent with no plan body, so deleted ids stay until reused.
    std::vector<std::int32_t> cleared_equip_plans;
    std::int32_t sign_count{};       // BaseInfo.SignInCount, next 七日签到 index
    std::int32_t last_sign_time{};   // BaseInfo.LastSignInTime
    std::int64_t sign_start{};       // BaseInfo.SignInStartTime; 0 keeps the panel closed
    std::int32_t divination_check{}; // bitmask, day 1 is bit 0
    std::int32_t divination_stamp{}; // MMDD the bitmask belongs to
    std::int32_t divination_time{};
    std::int32_t birthday{};         // month * 100 + day
    // In-progress fight (C2L_FightData until checkout). Login.fightDataProfile
    // only when SectionTable.isSaveGame != E_No; tutorial series is E_No.
    std::int32_t pending_section{};
    std::int32_t pending_chapter{};
    std::vector<AccountMail> mails;
    // 白夜行星建筑状态: buildingId -> {level, star}。701-708 缺省用 CollegeBuilding
    // 表 InitialLevel/InitialStar(1/1)；奇迹 721+ 缺省未建造(不在 civilization 下发)。
    std::map<std::int32_t, std::pair<std::int32_t, std::int32_t>> college;
    CollegeBuild build_queue; // 升级进行中: 建造时间走 CollegeLevel.f8, 到期完成
    // 白夜补给 / 随机商店 (801) 与外观商店持久化状态
    std::int32_t shop_refresh_times{}; // 今日已手动刷新次数 (0..3)
    std::int32_t shop_refresh_day{};   // 记录刷新次数的游戏天数
    std::map<std::int32_t, std::int32_t> shop_goods_bought; // goodsId -> 当日/当前批次已购买次数
    std::vector<std::int32_t> owned_skins; // 外观商店已购买的商品 GoodsID
};

struct Account {
    std::int64_t id{1};
    std::int32_t login_count{};
    bool is_create_role{true};
    AccountPlayer player;
    std::vector<AccountHero> heroes;
    std::vector<AccountItem> items;
};

} // namespace x2::offline
