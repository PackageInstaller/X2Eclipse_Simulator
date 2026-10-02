#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "offline/account_state.hpp"
#include "offline/game_table.hpp"
#include "offline/medal.hpp"
#include "offline/table_blob.hpp"

namespace x2::offline {


struct CheckoutRequest {
    std::int32_t chapter{};
    std::int32_t section{};
    bool success{};
    std::vector<std::int32_t> heroes;
    std::vector<AccountItem> maze_items;
    std::int32_t kills{};
    bool expert_mode{};
};

[[nodiscard]] CheckoutRequest parse_checkout(std::span<const std::uint8_t> body);

struct CheckoutResult {
    std::vector<AccountItem> rewards;         // RewardData.rewardItem
    std::vector<AccountEquip> reward_equips;  // RewardData.rewardEquip + L2C_EquipUpdate
    std::vector<AccountItem> changed_items;   // bag entries to push (L2C_ItemUpdate)
    std::vector<AccountHero> changed_heroes;  // Item.functionEff E_Hero + L2C_HeroUpdate
    std::int32_t level_ups{};
    std::int32_t returned_power{};
    bool first_clear{};
};


struct HeroOptRequest {
    std::int32_t id{};
    std::int32_t opt{};
    std::int32_t consume_item{};
    std::string name;
};

struct HeroOptResult {
    std::int32_t code{10}; // GameLogicErrCode.E_Ok
    std::vector<AccountHero> heroes;
    std::vector<AccountItem> items;
    bool player{};
};


enum CurrencyEnu : std::int32_t {
    E_Power = 0, E_Gold = 1, E_Money = 2, E_Silver = 3, E_JewelChip = 4,
    E_EquibExp = 6, E_HeroExp = 7, E_RoleExp = 8, E_DailyActiveValue = 10,
    E_PowerOfLight = 13, E_VowOfCoin = 14, E_Crystal = 15, E_SkillPoint = 16,
    E_FriendCoin = 17, E_BossCoin = 18,
    E_SkinTicket = 23,
};

[[nodiscard]] std::int32_t* wallet(AccountPlayer& player, std::int32_t enu);
[[nodiscard]] std::optional<std::int32_t> item_currency_enu(const TableBlob* tables, std::int32_t item_id);
[[nodiscard]] bool is_currency_item(const TableBlob* tables, std::int32_t item_id);
[[nodiscard]] std::int32_t clamp_add(std::int32_t base, std::int64_t delta);
void expand_gift(const GameTable& gift, std::int32_t group, std::vector<AccountItem>& out);

struct Paid {
    bool player{};
    bool role_exp{};
    std::vector<AccountItem> shown;
    std::vector<AccountItem> bag;
    std::vector<AccountEquip> equips; // RewardData.rewardEquip + L2C_EquipUpdate
};

[[nodiscard]] Paid pay_rewards(Account& account, const TableBlob* tables, const std::vector<AccountItem>& rewards);
[[nodiscard]] std::int32_t section_power_cost(const TableBlob& tables, std::int32_t section);
[[nodiscard]] std::vector<std::int32_t> drop_values(const TableBlob& tables, std::int32_t section);
[[nodiscard]] CheckoutResult apply_checkout(Account& account, const TableBlob& tables, const CheckoutRequest& request);
bool drop_maze_items(Account& account, const TableBlob& tables);
void seed_guides(Account& account, const TableBlob& tables);
[[nodiscard]] std::vector<std::int32_t> unlocked_stars(const Account& account, const TableBlob& tables);

struct TaskProgress {
    std::int32_t current{};
    std::int32_t target{1};
};

[[nodiscard]] TaskProgress task_progress(const Account& account, const TableBlob& tables, std::int32_t condition_id);
[[nodiscard]] std::vector<std::uint8_t> challenge_task_reply(const Account& account, const TableBlob& tables);
struct StarSkillUp {
    bool ok{};
};

[[nodiscard]] StarSkillUp upgrade_star_skill(Account& account, const TableBlob* tables, std::int32_t skill_id,
                                             std::int32_t target_level);


void finish_guide(Account& account, std::int32_t group, std::int32_t next_group);

// L2C_CheckoutMainMission body.
[[nodiscard]] std::vector<std::uint8_t> checkout_reply(const Account& account, const CheckoutRequest& request,
                                                       const CheckoutResult& result);

[[nodiscard]] std::int32_t default_hero_star(const TableBlob* tables, std::int32_t hero_id);
[[nodiscard]] std::int32_t max_hero_star(const TableBlob* tables);
void grant_role_level_gifts(Account& account, const TableBlob* tables, std::int32_t from_level, std::int32_t to_level);

// C2L_GetCollectionAward. already means it was claimed before; shown is the popup list.
struct CollectionClaim {
    bool ok{};
    bool already{};
    bool player{};
    std::vector<AccountItem> shown;
    std::vector<AccountItem> bag;
};

[[nodiscard]] CollectionClaim claim_collection_award(Account& account, const TableBlob* tables, std::int32_t award_id);


void roll_daily_tasks(Account& account, const TableBlob& tables, std::int64_t now);
void count_daily_task(Account& account, const TableBlob& tables, std::int32_t type, std::int32_t value,
                      std::int32_t n = 1);


[[nodiscard]] std::vector<std::uint8_t> daily_task_reply(Account& account, const TableBlob& tables);

struct TaskClaim {
    bool ok{};
    bool already{};
    bool player{};
    std::int32_t level_ups{};
    std::vector<AccountItem> shown;
    std::vector<AccountItem> bag;
    std::vector<AccountEquip> equips; // RewardData.rewardEquip + L2C_EquipUpdate
};

[[nodiscard]] TaskClaim claim_daily_task(Account& account, const TableBlob* tables, std::int32_t task_id);
[[nodiscard]] TaskClaim pick_daily_box(Account& account, const TableBlob* tables, std::int32_t box_id);
struct EquipWear {
    bool ok{};
    std::vector<AccountHero> heroes;
};

struct EquipStrengthenResult {
    bool ok{false};
    std::int32_t code{10}; // LogicCode::Ok = 10
    std::int32_t equip_id{};
    std::int32_t new_level{};
    AccountEquip updated_equip;
};

[[nodiscard]] EquipStrengthenResult apply_equip_strengthen(Account& account, const TableBlob* tables, std::int32_t equip_id);

[[nodiscard]] EquipWear wear_equip(Account& account, const TableBlob* tables, std::int32_t equip_id, std::int32_t hero_id);
[[nodiscard]] EquipWear unequip(Account& account, std::int32_t hero_id, std::int32_t pos);
bool fix_worn_slots(Account& account, const TableBlob& tables);
[[nodiscard]] std::int32_t save_equip_plan(Account& account, std::int32_t id, std::string name,
                                           std::map<std::int32_t, std::int32_t> positions);
bool delete_equip_plan(Account& account, std::int32_t id);

[[nodiscard]] TaskClaim claim_challenge_task(Account& account, const TableBlob* tables, std::int32_t task_id);
[[nodiscard]] TaskClaim claim_rookie_sign(Account& account, const TableBlob* tables, std::int32_t index,
                                          std::int32_t now);
[[nodiscard]] TaskClaim claim_divination(Account& account, const TableBlob* tables, std::int32_t now);
struct DivinationView {
    std::int32_t id{};
    std::int32_t check_in{};
    std::int32_t valid{};
};
[[nodiscard]] DivinationView divination_view(Account& account, const TableBlob* tables, std::int32_t now);
[[nodiscard]] TaskClaim pick_challenge_box(Account& account, const TableBlob* tables, std::int32_t phase);
[[nodiscard]] TaskClaim use_item(Account& account, const TableBlob* tables, std::int32_t item_id, std::int32_t count,
                                 std::vector<std::int32_t> picks);

inline constexpr std::int32_t kOpenPools[] = {22201, 22203};

struct DrawCard {
    std::int32_t item_id{};
    std::int32_t num{};
    std::int32_t hero_id{};
    bool transform{};
};

struct LuckDraw {
    bool ok{};
    std::int32_t level_ups{};
    std::vector<DrawCard> cards;
    std::vector<AccountItem> bag;
    std::vector<AccountHero> heroes; // new units (L2C_HeroUpdate)
};


[[nodiscard]] LuckDraw luck_draw(Account& account, const TableBlob* tables, std::int32_t pool_id,
                                 std::int32_t draw_type);

inline constexpr std::int32_t kCourseActivity = 100002;
[[nodiscard]] TaskClaim claim_course(Account& account, const TableBlob* tables, const std::vector<std::int32_t>& levels);

[[nodiscard]] std::int32_t max_role_level(const TableBlob* tables);

[[nodiscard]] HeroOptRequest parse_hero_opt(std::span<const std::uint8_t> body);

[[nodiscard]] HeroOptResult apply_hero_opt(Account& account, const TableBlob* tables, const HeroOptRequest& request);

[[nodiscard]] HeroOptResult apply_god_like(Account& account, const TableBlob* tables, std::int32_t hero_id);

[[nodiscard]] HeroOptResult apply_god_slot(Account& account, const TableBlob* tables, std::int32_t hero_id,
                                           std::int32_t slot);

[[nodiscard]] HeroOptResult apply_hero_skill_up(Account& account, const TableBlob* tables,
                                                std::int32_t hero_id, std::int32_t skill_id,
                                                std::int32_t uplevel);

struct ArtifactResult {
    bool ok{};
    std::int32_t code{0}; // 0 = Ok
    std::vector<AccountHero> heroes;
    std::vector<AccountItem> items;
    bool player_currency_changed{};
};

// C2L_Artifact opt: 0 = JO_LevelUP, 1 = JO_Fuse, 2 = JO_Beset
[[nodiscard]] ArtifactResult handle_artifact_opt(Account& account, const TableBlob* tables,
                                                std::int32_t opt, std::int32_t hero_id,
                                                std::int32_t jewel_id, std::int32_t hole_id);

struct AddFavorRequest {
    std::int32_t opt{2};      // AddFavorOpt: 2 = SendGift
    std::int32_t option_id{}; // Item ID
    std::int32_t hero_id{};   // Hero ID
    std::int32_t num{1};      // Count
};

struct AddFavorResult {
    bool ok{false};
    std::int32_t code{10}; // LogicCode::Ok = 10
    std::int32_t opt{2};
    std::int32_t option_id{};
    std::int32_t hero_id{};
    std::int32_t before_level{};
    std::int32_t before_exp{};
    std::int32_t after_level{};
    std::int32_t after_exp{};
    std::int32_t gifts_times{};
    std::int32_t item_id{};
    std::int32_t new_item_count{};
    AccountHero updated_hero;
    std::vector<std::int32_t> unlocked_dubbings;
};

// C2L_AddFavor. opt: 1 = TouchInteraction, 2 = SendGift
[[nodiscard]] AddFavorResult apply_add_favor(Account& account, const TableBlob* tables,
                                            const AddFavorRequest& request);

struct UpgradeFettersRequest {
    std::int32_t hero_id{};
    std::int32_t position_id{};
};

struct UpgradeFettersResult {
    bool ok{false};
    std::int32_t code{10}; // LogicCode::Ok = 10
    std::int32_t hero_id{};
    std::int32_t position_id{};
    std::int32_t new_level{};
    AccountHero updated_hero;
    bool gold_changed{false};
};

[[nodiscard]] UpgradeFettersResult apply_upgrade_fetters(Account& account, const TableBlob* tables,
                                                         const UpgradeFettersRequest& request);

struct FavorBreakRequest {
    std::int32_t hero_id{};
};

struct FavorBreakResult {
    bool ok{false};
    std::int32_t code{10}; // LogicCode::Ok = 10
    std::int32_t hero_id{};
    std::int32_t before_level{};
    std::int32_t after_level{};
    AccountHero updated_hero;
    std::vector<AccountItem> changed_items;
    std::vector<std::int32_t> unlocked_dubbings;
};

[[nodiscard]] FavorBreakResult apply_favor_break(Account& account, const TableBlob* tables,
                                                 const FavorBreakRequest& request);


[[nodiscard]] std::vector<std::int32_t> dubbing_unlock_ids(const AccountHero& hero, const TableBlob* tables);

// 白夜行星
inline constexpr std::int32_t kCollegeOk = 10;
inline constexpr std::int32_t kCollegeNoItem = 22;
inline constexpr std::int32_t kCollegeLimitGold = 35; 
inline constexpr std::int32_t kCollegeBusy = 55;
inline constexpr std::int32_t kCollegeMaxLevel = 14;

struct CollegeUpgradeResult {
    bool ok{false};
    std::int32_t code{kCollegeOk};
    std::int32_t building_id{};
    std::int32_t type{};
    std::int32_t level{};
    std::int64_t end_unix{};
    std::int32_t duration_sec{};
    std::vector<AccountItem> changed_items;
    bool wallet_changed{false};
};
[[nodiscard]] CollegeUpgradeResult start_college_upgrade(Account& account, const TableBlob* tables,
                                                         std::int32_t building_id, std::int32_t now_unix);

struct CollegeSpeedResult {
    bool ok{false};
    std::int32_t code{kCollegeOk};
    std::int32_t building_id{};
    std::int32_t type{};
    std::int32_t item_id{};
    std::int32_t item_num{};
    std::vector<AccountItem> changed_items;
    bool wallet_changed{false};
};
[[nodiscard]] CollegeSpeedResult speed_up_college_build(Account& account, const TableBlob* tables,
                                                        std::int32_t building_id, std::int32_t item_id,
                                                        std::int32_t item_num, std::int32_t now_unix);

struct CollegeStarResult {
    bool ok{false};
    std::int32_t code{kCollegeOk};
    std::int32_t building_id{};
    std::int32_t type{};
    std::int32_t star{};       // star reached
    std::vector<AccountItem> changed_items;
    bool wallet_changed{false};
};
[[nodiscard]] CollegeStarResult star_up_college(Account& account, const TableBlob* tables,
                                                std::int32_t building_id);

[[nodiscard]] std::int32_t finish_expired_college_build(Account& account, std::int32_t now_unix);

[[nodiscard]] std::pair<std::int32_t, std::int32_t> college_state(const Account& account,
                                                                  const TableBlob* tables,
                                                                  std::int32_t building_id);

} // namespace x2::offline
