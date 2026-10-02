#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <map>
#include <set>

#include "offline/account_state.hpp"
#include "offline/table_blob.hpp"

namespace x2::offline {

struct ShopItemDef {
    std::int32_t goods_id{};
    std::int32_t group_id{};
    std::int32_t original_price{};
    std::int32_t item_id{};
    std::int32_t num{};
    std::int32_t price{};
    std::int32_t currency_type{};
    std::int32_t can_buy_times{};
    std::int32_t goods_tag{};
    std::int32_t limited{};
};

struct ShopGoodsResult {
    bool ok{true};
    std::int32_t code{10};
    std::int32_t shop_id{801};
    std::int32_t refresh_times{0};
    std::int32_t refresh_price{50};
    std::vector<std::uint8_t> body; // Encoded L2C_ShopGoods or L2C_RefreshShop body
    bool wallet_changed{false};
};

struct BuyGoodsResult {
    bool ok{false};
    std::int32_t code{13};
    std::int32_t item_id{0};
    std::int32_t item_num{0};
    std::vector<AccountItem> changed_items;
    bool wallet_changed{false};
    std::vector<std::uint8_t> body; // Encoded L2C_BuyGoods body
};

struct CommercialGoodsResult {
    bool ok{true};
    std::int32_t code{10};
    std::int32_t shop_type{1};
    std::vector<std::uint8_t> body; // Encoded L2C_CommercialShopGoods body
};

struct BuyCommercialResult {
    bool ok{false};
    std::int32_t code{13};
    std::int32_t goods_id{0};
    std::int32_t item_id{0};
    std::vector<AccountItem> changed_items;
    bool wallet_changed{false};
    std::vector<std::uint8_t> body; // Encoded L2C_BuyCommercialGoods body
};

// 检查并更新每日随机商店的刷新状态（跨天自动重置刷新次数和已购记录）
void update_shop_daily(Account& account, std::int64_t now_unix);

// 查询商店商品列表 (ShopID: 801 为随机商店)
ShopGoodsResult query_shop_goods(Account& account, const TableBlob* tables, std::int32_t shop_id, std::int64_t now_unix);

// 手动刷新商店 (ShopID: 801) - 方案A: 梯级消耗 50, 50, 100 光辉，每日上限 3 次
ShopGoodsResult refresh_shop_goods(Account& account, const TableBlob* tables, std::int32_t shop_id, std::int64_t now_unix);

// 购买普通商品 (ShopID: 801)
BuyGoodsResult buy_shop_goods(Account& account, const TableBlob* tables, std::int32_t shop_id, std::int32_t goods_id, std::int32_t buy_num);

// 查询外观商店 (shop_type: 1 为 SKIN)
CommercialGoodsResult query_commercial_goods(const Account& account, const TableBlob* tables, std::int32_t shop_type);

// 购买外观商品 (皮肤)
BuyCommercialResult buy_commercial_goods(Account& account, const TableBlob* tables, std::int32_t goods_id, std::int32_t shop_type, std::int32_t currency_type);

// heroId -> 拥有的外观皮肤 id 集合 (UnitBase 初始 + 已购/背包皮肤道具)
std::map<std::int32_t, std::set<std::int32_t>> collect_hero_skins(const Account& account, const TableBlob* tables);

// 查询神格外观皮肤全量数据 (L2C_HeroSkinAll)
std::vector<std::uint8_t> build_hero_skin_all(const Account& account, const TableBlob* tables);

// 组装单神格外观皮肤增量数据 (L2C_HeroSkinUpdate)
std::vector<std::uint8_t> build_hero_skin_update(std::int32_t hero_id, const Account& account, const TableBlob* tables);

// 组装推荐商店回包 (L2C_QueryReCommendShop)
std::vector<std::uint8_t> build_recommend_shop(const Account& account, const TableBlob* tables);

} // namespace x2::offline
