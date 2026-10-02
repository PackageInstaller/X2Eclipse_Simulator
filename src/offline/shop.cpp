#include "offline/shop.hpp"

#include <algorithm>
#include <ctime>
#include <map>
#include <set>
#include <vector>

#include "offline/game_table.hpp"
#include "offline/mission.hpp"
#include "proto/protobuf.hpp"
#include "server/protocol_ids.hpp"

namespace x2::offline {

namespace {

using proto::Writer;

constexpr std::int64_t kServerUtcOffset = 8 * 3600; // UTC+8
constexpr std::int64_t kDailyResetHour = 5;         // 每日凌晨 5:00 重置
constexpr std::int32_t kMaxRefreshTimes = 3;        // 每日最大刷新次数

// ShopConfig 801 的 RefreshPrice [50, 50, 100, 200, 400, 600]
std::int32_t get_refresh_cost(std::int32_t refresh_times) {
    if (refresh_times == 0) return 50;
    if (refresh_times == 1) return 50;
    if (refresh_times == 2) return 100;
    return 200;
}

// 随机商店 801
static constexpr ShopItemDef kDefaultShop801Goods[] = {
    {1900101, 1,  20,     1237914, 1,     20,     902, 3,  3, 3}, // 许愿币 x1 (20光辉, 限购3次)
    {1900201, 2,  60,     1237900, 60,    60,     902, 10, 3, 0}, // 因果 x60 (60光辉, 限购10次)
    {1900311, 3,  60000,  1237906, 1000,  60000,  901, 1,  2, 0}, // 兽魂 x1000 (60000金币)
    {1900401, 4,  90,     1237901, 20000, 90,     902, 1,  2, 0}, // 金币 x20000 (90光辉)
    {1900501, 5,  12500,  1202080, 2,     12500,  901, 1,  3, 0}, // 神权拓本·空白 x2 (12500金币)
    {1900601, 6,  12500,  1202081, 2,     12500,  901, 1,  3, 0}, // 神权拓本·剑柄 x2 (12500金币)
    {1900701, 7,  12500,  1202082, 2,     12500,  901, 1,  3, 0}, // 神权拓本·圣杯 x2 (12500金币)
    {1900801, 8,  12500,  1202083, 2,     12500,  901, 1,  3, 0}, // 神权拓本·星币 x2 (12500金币)
    {1900901, 9,  30000,  1202084, 2,     30000,  901, 1,  2, 0}, // 神权拓本·权杖 x2 (30000金币)
    {1901001, 10, 60000,  1237831, 6,     60000,  901, 12, 2, 0}, // 加速卡:10分钟 x6 (60000金币)
    {1901101, 11, 150000, 1237832, 6,     150000, 901, 2,  2, 0}, // 加速卡:1小时 x6 (150000金币)
    {1901201, 12, 50000,  1237833, 6,     50000,  901, 1,  2, 0}, // 加速卡:2小时 x6 (50000金币)
    {1901301, 13, 50000,  1237834, 3,     50000,  901, 1,  2, 0}, // 加速卡:4小时 x3 (50000金币)
    {1901401, 14, 6000,   1238050, 3,     6000,   901, 3,  2, 0}, // 金粒子 x3 (6000金币)
    {1901501, 15, 300,    1238051, 3,     300,    913, 1,  3, 0}, // 水晶粒子 x3 (300光能)
    {1901601, 16, 180,    1238060, 3,     180,    902, 3,  3, 0}, // 金剑柄 x3 (180光辉)
    {1901701, 23, 60,     1237804, 12,    60,     902, 2,  3, 2}, // 4★炼金土 x12 (60光辉)
    {1901702, 24, 80,     1237805, 12,    80,     902, 1,  3, 2}, // 5★聚灵木 x12 (80光辉)
};

std::vector<ShopItemDef> get_shop_items(const TableBlob* tables, std::int32_t shop_id) {
    std::vector<ShopItemDef> items;
    if (shop_id == 801) {
        items.assign(std::begin(kDefaultShop801Goods), std::end(kDefaultShop801Goods));
        if (tables) {
            GameTable abg, sgg;
            if (abg.load(*tables, "ActivityBoxGoods") && sgg.load(*tables, "ShopGoodsGroup")) {
                for (auto& item : items) {
                    if (const auto sgg_row = sgg.row(static_cast<std::uint64_t>(item.goods_id))) {
                        item.group_id = sgg.int_field(*sgg_row, 2).value_or(item.group_id);
                        item.currency_type = sgg.int_field(*sgg_row, 3).value_or(item.currency_type);
                        item.price = sgg.int_field(*sgg_row, 4).value_or(item.price);
                        item.goods_tag = sgg.int_field(*sgg_row, 8).value_or(item.goods_tag);
                        item.limited = sgg.int_field(*sgg_row, 9).value_or(item.limited);
                        item.original_price = (item.goods_tag == 2) ? item.price * 10 / 8 : item.price;
                    }
                    if (const auto abg_row = abg.row(static_cast<std::uint64_t>(item.group_id))) {
                        item.item_id = abg.int_field(*abg_row, 2).value_or(item.item_id);
                        item.num = abg.int_field(*abg_row, 3).value_or(item.num);
                        item.can_buy_times = abg.int_field(*abg_row, 4).value_or(item.can_buy_times);
                    }
                }
            }
        }
        return items;
    }
    if (!tables) return items;
    GameTable cfg, abg, sgg;
    if (!cfg.load(*tables, "ShopConfig") || !abg.load(*tables, "ActivityBoxGoods") ||
        !sgg.load(*tables, "ShopGoodsGroup")) {
        return items;
    }
    const auto cfg_row = cfg.row(static_cast<std::uint64_t>(shop_id));
    if (!cfg_row) return items;
    for (const auto group : cfg.ints_field(*cfg_row, 14)) {
        const auto abg_row = abg.row(static_cast<std::uint64_t>(group));
        if (!abg_row) continue;
        for (const auto& sgg_row : sgg.rows()) {
            if (sgg.int_field(sgg_row, 2).value_or(0) != group) continue;
            ShopItemDef item;
            item.goods_id = static_cast<std::int32_t>(sgg_row.key);
            item.group_id = group;
            item.currency_type = sgg.int_field(sgg_row, 3).value_or(917);
            item.price = sgg.int_field(sgg_row, 4).value_or(0);
            item.goods_tag = sgg.int_field(sgg_row, 8).value_or(0);
            item.limited = sgg.int_field(sgg_row, 9).value_or(0);
            item.original_price = (item.goods_tag == 2) ? item.price * 10 / 8 : item.price;
            item.item_id = abg.int_field(*abg_row, 2).value_or(0);
            item.num = abg.int_field(*abg_row, 3).value_or(1);
            item.can_buy_times = abg.int_field(*abg_row, 4).value_or(1);
            if (item.item_id > 0) items.push_back(item);
        }
    }
    return items;
}

std::vector<std::uint8_t> encode_goods_item(const ShopItemDef& item, std::int32_t has_buy_times) {
    Writer g;
    g.int32(1, item.goods_id);
    g.int32(2, item.original_price);
    g.int32(3, item.item_id);
    g.int32(4, item.num);
    g.int32(5, item.price);
    g.int32(6, item.currency_type);
    g.int32(7, item.can_buy_times);
    g.int32(8, has_buy_times);
    g.int32(9, 0); // startTime
    g.int32(10, 0); // endTime
    g.int32(11, item.goods_tag);
    g.int32(12, item.limited);
    return g.data();
}

} // namespace

void update_shop_daily(Account& account, std::int64_t now_unix) {
    const auto day = static_cast<std::int32_t>((now_unix + kServerUtcOffset - kDailyResetHour * 3600) / 86400);
    if (account.player.shop_refresh_day != day) {
        account.player.shop_refresh_day = day;
        account.player.shop_refresh_times = 0;
        account.player.shop_goods_bought.clear();
    }
}

ShopGoodsResult query_shop_goods(Account& account, const TableBlob* tables, std::int32_t shop_id, std::int64_t now_unix) {
    update_shop_daily(account, now_unix);

    const auto items = get_shop_items(tables, shop_id);
    const auto refresh_times = (shop_id == 801) ? account.player.shop_refresh_times : 0;
    const auto refresh_price = (shop_id == 801) ? get_refresh_cost(refresh_times) : 0;

    Writer body;
    body.int32(1, static_cast<std::int32_t>(server::LogicCode::Ok)); // 1: code
    body.int32(2, shop_id);                                           // 2: shopId
    body.int64(3, 0);                                                 // 3: NextRefreshTime
    body.int32(4, refresh_times);                                     // 4: RefreshTimes
    body.int32(5, refresh_price);                                     // 5: RefreshPrice
    for (const auto& item : items) {
        const auto bought_it = account.player.shop_goods_bought.find(item.goods_id);
        const auto has_buy = (bought_it != account.player.shop_goods_bought.end()) ? bought_it->second : 0;
        body.message(6, encode_goods_item(item, has_buy));            // 6: goods
    }

    return {
        .ok = true,
        .code = static_cast<std::int32_t>(server::LogicCode::Ok),
        .shop_id = shop_id,
        .refresh_times = refresh_times,
        .refresh_price = refresh_price,
        .body = body.data(),
        .wallet_changed = false,
    };
}

ShopGoodsResult refresh_shop_goods(Account& account, const TableBlob* tables, std::int32_t shop_id, std::int64_t now_unix) {
    update_shop_daily(account, now_unix);

    if (shop_id != 801) {
        return {.ok = false, .code = static_cast<std::int32_t>(server::LogicCode::ErrorOpt)};
    }

    if (account.player.shop_refresh_times >= kMaxRefreshTimes) {
        return {.ok = false, .code = static_cast<std::int32_t>(server::LogicCode::ErrorOpt)};
    }

    const auto cost = get_refresh_cost(account.player.shop_refresh_times);
    if (account.player.crystal < cost) {
        return {.ok = false, .code = static_cast<std::int32_t>(server::LogicCode::ErrorOpt)};
    }

    // 扣除货币并推进刷新次数
    account.player.crystal -= cost;
    account.player.shop_refresh_times++;
    account.player.shop_goods_bought.clear();

    const auto items = get_shop_items(tables, shop_id);
    const auto next_cost = get_refresh_cost(account.player.shop_refresh_times);

    Writer body;
    body.int32(1, static_cast<std::int32_t>(server::LogicCode::Ok)); // 1: code
    body.int32(2, shop_id);                                           // 2: shopId
    body.int64(3, 0);                                                 // 3: NextRefreshTime
    body.int32(4, account.player.shop_refresh_times);                 // 4: RefreshTimes
    body.int32(5, next_cost);                                         // 5: RefreshPrice
    for (const auto& item : items) {
        body.message(6, encode_goods_item(item, 0));                  // 6: goods (已购全清空)
    }

    return {
        .ok = true,
        .code = static_cast<std::int32_t>(server::LogicCode::Ok),
        .shop_id = shop_id,
        .refresh_times = account.player.shop_refresh_times,
        .refresh_price = next_cost,
        .body = body.data(),
        .wallet_changed = true,
    };
}

BuyGoodsResult buy_shop_goods(Account& account, const TableBlob* tables, std::int32_t shop_id, std::int32_t goods_id, std::int32_t buy_num) {
    if (buy_num <= 0) return {.ok = false, .code = static_cast<std::int32_t>(server::LogicCode::ErrorOpt)};

    const auto items = get_shop_items(tables, shop_id);
    const auto it = std::ranges::find(items, goods_id, &ShopItemDef::goods_id);
    if (it == items.end()) return {.ok = false, .code = static_cast<std::int32_t>(server::LogicCode::ErrorOpt)};

    const auto& good = *it;
    const auto bought_it = account.player.shop_goods_bought.find(goods_id);
    const auto has_buy = (bought_it != account.player.shop_goods_bought.end()) ? bought_it->second : 0;

    if (good.can_buy_times > 0 && (has_buy + buy_num > good.can_buy_times)) {
        return {.ok = false, .code = static_cast<std::int32_t>(server::LogicCode::ErrorOpt)};
    }

    const auto total_cost = good.price * buy_num;
    bool wallet_changed = false;
    std::vector<AccountItem> changed_items;

    {
        std::optional<std::int32_t> enu;
        if (tables) {
            GameTable currency;
            if (currency.load(*tables, "CurrencyType")) {
                if (const auto row = currency.row(static_cast<std::uint64_t>(good.currency_type))) {
                    enu = currency.int_field(*row, 4).value_or(0);
                }
            }
        }
        auto* field = enu ? wallet(account.player, *enu) : nullptr;
        if (field == nullptr) return {.ok = false, .code = static_cast<std::int32_t>(server::LogicCode::ErrorOpt)};
        if (*field < total_cost) return {.ok = false, .code = static_cast<std::int32_t>(server::LogicCode::ErrorOpt)};
        *field -= total_cost;
        wallet_changed = true;
    }

    // 记录购买次数
    const auto new_has_buy = has_buy + buy_num;
    account.player.shop_goods_bought[goods_id] = new_has_buy;

    // 发放商品道具: 货币类道具按 enu 入钱包, 其余入背包
    const auto total_item_num = good.num * buy_num;
    if (const auto grant_enu = item_currency_enu(tables, good.item_id)) {
        if (auto* field = wallet(account.player, *grant_enu)) {
            *field = clamp_add(*field, total_item_num);
            wallet_changed = true;
        }
    } else if (good.item_id == 1237901) { // 金币
        account.player.gold += total_item_num;
        wallet_changed = true;
    } else if (good.item_id == 1237900) { // 因果 (体力)
        account.player.power += total_item_num;
        wallet_changed = true;
    } else if (good.item_id == 1237914) { // 许愿币
        account.player.vow_coin += total_item_num;
        wallet_changed = true;
    } else {
        auto bag_it = std::ranges::find(account.items, good.item_id, &AccountItem::id);
        if (bag_it == account.items.end()) {
            account.items.push_back(AccountItem{.id = good.item_id, .num = total_item_num, .locked = false});
            changed_items.push_back(account.items.back());
        } else {
            bag_it->num += total_item_num;
            changed_items.push_back(*bag_it);
        }
    }

    // 组装 L2C_BuyGoods 回包 (Proto 222)
    Writer body;
    body.int32(1, static_cast<std::int32_t>(server::LogicCode::Ok)); // 1: code
    body.int32(2, good.item_id);                                     // 2: itemId
    body.int32(3, total_item_num);                                   // 3: itemNum
    body.int32(4, goods_id);                                         // 4: goodsId
    body.int32(5, good.price);                                       // 5: price
    body.int32(6, good.original_price);                              // 6: originalPrice
    body.int32(7, new_has_buy);                                      // 7: hasBuyTimes
    {
        Writer reward_w;
        Writer item_w;
        item_w.int32(1, good.item_id);
        item_w.int32(2, total_item_num);
        item_w.boolean(3, false);                                    // 3: transform
        reward_w.message(1, item_w.data());                          // 1: repeated RewardItem
        body.message(8, reward_w.data());                            // 8: rewardData
    }
    body.int32(9, buy_num);                                          // 9: buyNum
    body.int32(10, 0);                                               // 10: changeItemID
    body.int32(11, shop_id);                                         // 11: shopId

    return {
        .ok = true,
        .code = static_cast<std::int32_t>(server::LogicCode::Ok),
        .item_id = good.item_id,
        .item_num = total_item_num,
        .changed_items = std::move(changed_items),
        .wallet_changed = wallet_changed,
        .body = body.data(),
    };
}

CommercialGoodsResult query_commercial_goods(const Account& account, const TableBlob* tables, std::int32_t shop_type) {
    Writer body;
    body.int32(1, static_cast<std::int32_t>(server::LogicCode::Ok)); // 1: code

    if (tables && (shop_type == 1)) { // SKIN
        GameTable app_shop;
        if (app_shop.load(*tables, "AppearanceShop")) {
            std::set<std::int32_t> seen_items;
            for (const auto& r : app_shop.rows()) {
                const auto goods_id = app_shop.int_field(r, 1).value_or(0);
                const auto item_id = app_shop.int_field(r, 2).value_or(0);

                // 排除白皇后 (1220803) 与 夏日花火 (1221903)
                if (item_id == 1220803 || item_id == 1221903) continue;

                if (seen_items.contains(item_id)) continue;
                seen_items.insert(item_id);

                const auto res_types = app_shop.ints_field(r, 3);
                const auto prices = app_shop.ints_field(r, 4);
                const auto prime_prices = app_shop.ints_field(r, 5);
                const auto goods_type = app_shop.int_field(r, 6).value_or(0);
                const auto recharge_id = app_shop.int_field(r, 7).value_or(0);

                const bool already_bought = std::ranges::contains(account.player.owned_skins, goods_id) ||
                                            std::ranges::contains(account.items, item_id, &AccountItem::id);

                Writer g;
                g.int32(1, goods_id);
                g.int32(2, 0); // startTime
                g.int32(3, 0); // endTime
                for (const auto p : prime_prices) g.int32(4, p); // originalPrice (unpacked repeated varint)
                g.int32(5, item_id);
                for (const auto p : prices) g.int32(6, p);       // price (unpacked repeated varint)
                for (const auto c : res_types) g.int32(7, c);    // currencyType (unpacked repeated varint)
                g.boolean(8, already_bought);
                g.int32(9, goods_type);
                g.int32(10, 0); // preCount
                g.int32(11, recharge_id);
                body.message(2, g.data());                       // 2: repeated CommercialGoods
            }
        }
    }

    body.int32(3, shop_type); // 3: shopType

    return {
        .ok = true,
        .code = static_cast<std::int32_t>(server::LogicCode::Ok),
        .shop_type = shop_type,
        .body = body.data(),
    };
}

BuyCommercialResult buy_commercial_goods(Account& account, const TableBlob* tables, std::int32_t goods_id, std::int32_t shop_type, std::int32_t currency_type) {
    if (!tables || (shop_type != 1)) {
        return {.ok = false, .code = static_cast<std::int32_t>(server::LogicCode::ErrorOpt)};
    }

    GameTable app_shop;
    if (!app_shop.load(*tables, "AppearanceShop")) {
        return {.ok = false, .code = static_cast<std::int32_t>(server::LogicCode::ErrorOpt)};
    }

    const auto r = app_shop.row(static_cast<std::uint64_t>(goods_id));
    if (!r) return {.ok = false, .code = static_cast<std::int32_t>(server::LogicCode::ErrorOpt)};

    const auto item_id = app_shop.int_field(*r, 2).value_or(0);
    const auto res_types = app_shop.ints_field(*r, 3);
    const auto prices = app_shop.ints_field(*r, 4);

    // 校验是否已拥有
    if (std::ranges::contains(account.player.owned_skins, goods_id) ||
        std::ranges::contains(account.items, item_id, &AccountItem::id)) {
        return {.ok = false, .code = static_cast<std::int32_t>(server::LogicCode::ErrorOpt)};
    }

    // 寻找匹配货币类型的价格
    std::int32_t price = -1;
    for (std::size_t i = 0; i < res_types.size() && i < prices.size(); ++i) {
        if (res_types[i] == currency_type) {
            price = prices[i];
            break;
        }
    }
    if (price < 0 && !prices.empty()) {
        price = prices[0];
        if (!res_types.empty()) currency_type = res_types[0];
    }
    if (price < 0) return {.ok = false, .code = static_cast<std::int32_t>(server::LogicCode::ErrorOpt)};

    bool wallet_changed = false;
    std::vector<AccountItem> changed_items;

    if (currency_type == 902) { // 光辉
        if (account.player.crystal < price) return {.ok = false, .code = static_cast<std::int32_t>(server::LogicCode::ErrorOpt)};
        account.player.crystal -= price;
        wallet_changed = true;
    } else if (currency_type == 923) { // 外观券 (CurrencyType 923 -> BaseInfo.SkinCoupon)
        if (account.player.skin_coupon < price) {
            return {.ok = false, .code = static_cast<std::int32_t>(server::LogicCode::ErrorOpt)};
        }
        account.player.skin_coupon -= price;
        wallet_changed = true;
    } else {
        return {.ok = false, .code = static_cast<std::int32_t>(server::LogicCode::ErrorOpt)};
    }

    // 记录皮肤拥有状态
    account.player.owned_skins.push_back(goods_id);

    // 将皮肤道具放入背包
    auto bag_it = std::ranges::find(account.items, item_id, &AccountItem::id);
    if (bag_it == account.items.end()) {
        account.items.push_back(AccountItem{.id = item_id, .num = 1, .locked = false});
        changed_items.push_back(account.items.back());
    } else {
        bag_it->num += 1;
        changed_items.push_back(*bag_it);
    }

    // 组装 L2C_BuyCommercialGoods 回包 (Proto 526)
    Writer body;
    body.int32(1, static_cast<std::int32_t>(server::LogicCode::Ok)); // 1: code
    body.int32(2, goods_id);                                         // 2: goodsId
    {
        Writer reward_w;
        Writer item_w;
        item_w.int32(1, item_id);
        item_w.int32(2, 1);
        reward_w.message(1, item_w.data());                          // 1: repeated RewardItem
        body.message(3, reward_w.data());                            // 3: rewardData
    }
    body.int32(4, shop_type);                                        // 4: shopType

    return {
        .ok = true,
        .code = static_cast<std::int32_t>(server::LogicCode::Ok),
        .goods_id = goods_id,
        .item_id = item_id,
        .changed_items = std::move(changed_items),
        .wallet_changed = wallet_changed,
        .body = body.data(),
    };
}

namespace {

} // namespace

std::vector<std::uint8_t> build_hero_skin_all(const Account& account, const TableBlob* tables) {
    Writer body;
    const auto hero_skins = collect_hero_skins(account, tables);

    std::map<std::int32_t, const AccountHero*> hero_map;
    for (const auto& hero : account.heroes) {
        hero_map[hero.id] = &hero;
    }

    for (const auto& [hero_id, skins] : hero_skins) {
        Writer skin_entry;
        skin_entry.int32(1, hero_id);
        for (const auto sid : skins) skin_entry.int32(2, sid); // 2: repeated skinIds
        std::int32_t battle_skin = 0;
        std::int32_t outer_skin = 0;
        if (auto it = hero_map.find(hero_id); it != hero_map.end()) {
            battle_skin = it->second->battle_skin;
            outer_skin = it->second->outer_skin;
        }
        skin_entry.int32(3, battle_skin);                      // 3: battleSkin
        skin_entry.int32(4, outer_skin);                       // 4: outerSkin
        body.message(1, skin_entry.data());                    // 1: repeated HeroSkin
    }

    return body.data();
}

std::vector<std::uint8_t> build_hero_skin_update(std::int32_t hero_id, const Account& account, const TableBlob* tables) {
    Writer body;
    const auto hero_skins = collect_hero_skins(account, tables);

    Writer skin_entry;
    skin_entry.int32(1, hero_id);
    if (auto it = hero_skins.find(hero_id); it != hero_skins.end()) {
        for (const auto sid : it->second) skin_entry.int32(2, sid);
    } else {
        skin_entry.int32(2, 1220000 + (hero_id - 1000) * 100 + 1);
    }

    std::int32_t battle_skin = 0;
    std::int32_t outer_skin = 0;
    for (const auto& hero : account.heroes) {
        if (hero.id == hero_id) {
            battle_skin = hero.battle_skin;
            outer_skin = hero.outer_skin;
            break;
        }
    }
    skin_entry.int32(3, battle_skin);
    skin_entry.int32(4, outer_skin);

    body.message(1, skin_entry.data()); // 1: HeroSkin
    return body.data();
}

std::vector<std::uint8_t> build_recommend_shop(const Account& account, const TableBlob* tables) {
    Writer body;
    body.int32(1, static_cast<std::int32_t>(server::LogicCode::Ok)); // 1: code

    auto append_tag = [&](std::int32_t pos, std::string_view name, std::int32_t tag_flag, std::int32_t jump_id,
                          std::string_view banner_url) {
        Writer tag;
        tag.int32(1, 1);                     // 1: showType
        tag.int64(2, 0);                     // 2: startTime
        tag.int64(3, 0);                     // 3: endTime
        tag.int32(4, pos);                   // 4: order
        tag.int32(5, 0);                     // 5: type (0: 标准全横幅点击跳转)

        // 6: repeated hotAreaParam
        Writer area;
        area.uint64(1, 1000);                // 1: wide
        area.uint64(2, 500);                 // 2: high
        area.uint64(3, 0);                   // 3: xcoordinate
        area.uint64(4, 0);                   // 4: ycoordinate
        area.uint64(5, static_cast<std::uint64_t>(jump_id)); // 5: jumpId
        tag.message(6, area.data());

        tag.string(7, banner_url);           // 7: cdnLinkPic
        tag.string(8, name);                 // 8: name
        tag.int32(9, pos);                   // 9: pos
        tag.int32(10, tag_flag);             // 10: tag (1: 限时, 2: 新品)
        body.message(2, tag.data());         // 2: repeated RecommendTag
    };

    append_tag(1, "随机商店", 2, 29004, "http://127.0.0.1:9999/banner/shop_801.png");
    append_tag(2, "外观商店", 0, 29197, "http://127.0.0.1:9999/banner/shop_skin.png");
    append_tag(3, "勋章兑换", 0, 29103, "http://127.0.0.1:9999/banner/shop_803.png");

    return body.data();
}

std::map<std::int32_t, std::set<std::int32_t>> collect_hero_skins(const Account& account, const TableBlob* tables) {
    std::map<std::int32_t, std::set<std::int32_t>> hero_skins;

    if (tables) {
        GameTable unit_tbl, app_shop, item_tbl, app_tbl;
        const bool has_unit = unit_tbl.load(*tables, "UnitBase");
        const bool has_app_shop = app_shop.load(*tables, "AppearanceShop");
        const bool has_item = item_tbl.load(*tables, "Item");
        const bool has_app = app_tbl.load(*tables, "Appearance");

        // 1. 从 UnitBase 提取所有神格的默认初始皮肤（AppearanceList[0]）
        if (has_unit) {
            for (const auto& row : unit_tbl.rows()) {
                const auto hid = unit_tbl.int_field(row, 1).value_or(0);
                if (hid <= 0) continue;
                const auto apps = unit_tbl.ints_field(row, 48); // field 48: AppearanceList
                if (!apps.empty()) {
                    hero_skins[hid].insert(apps[0]);
                } else {
                    hero_skins[hid].insert(1220000 + (hid - 1000) * 100 + 1);
                }
            }
        }

        if (has_app) {
            for (const auto& hero : account.heroes) {
                if (hero.state != 2 || hero.star < 11) continue;
                for (const auto& ar : app_tbl.rows()) {
                    if (app_tbl.int_field(ar, 18).value_or(0) != hero.id) continue; // f18 HeroID
                    if (app_tbl.int_field(ar, 2).value_or(0) != 2) continue;        // f2 cond=E_Stage
                    hero_skins[hero.id].insert(static_cast<std::int32_t>(ar.key));
                }
            }
        }

        // 2. 扫描已购买外观与背包道具（包含白皇后 1220803、夏日花火 1221903）
        if (has_app_shop && has_item && has_app) {
            auto add_skin_by_item = [&](std::int32_t item_id) {
                if (const auto ir = item_tbl.row(static_cast<std::uint64_t>(item_id))) {
                    const auto eff = item_tbl.ints_field(*ir, 22); // effData
                    if (!eff.empty()) {
                        const auto app_id = eff[0];
                        if (const auto ar = app_tbl.row(static_cast<std::uint64_t>(app_id))) {
                            const auto hero_id = app_tbl.int_field(*ar, 18).value_or(0); // heroID
                            if (hero_id > 0) hero_skins[hero_id].insert(app_id);
                        }
                    }
                }
            };

            for (const auto goods_id : account.player.owned_skins) {
                if (const auto r = app_shop.row(static_cast<std::uint64_t>(goods_id))) {
                    const auto item_id = app_shop.int_field(*r, 2).value_or(0);
                    add_skin_by_item(item_id);
                }
            }

            // 同时也直接扫描背包道具中的皮肤外观
            for (const auto& item : account.items) {
                add_skin_by_item(item.id);
            }
        }
    }

    // 针对白板/无表保底：账号拥有的英雄至少拥有默认初始皮肤
    for (const auto& hero : account.heroes) {
        hero_skins[hero.id].insert(1220000 + (hero.id - 1000) * 100 + 1);
    }

    return hero_skins;
}

} // namespace x2::offline

