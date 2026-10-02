#pragma once

#include <cstdint>
#include <vector>

#include "offline/account_state.hpp"
#include "offline/table_blob.hpp"

namespace x2::offline {


// 该成就当前的统计进度
[[nodiscard]] std::int32_t achv_progress(std::int32_t achievement_id, const Account& account,
                                         const TableBlob* tables);

// 该成就的目标值
[[nodiscard]] std::int32_t achv_target(std::int32_t achievement_id, const TableBlob* tables);

// 该成就的阶段下标 (Line 型: 已达成的最高阶, 0-based; 单阶段: 完成 ? 1 : 0)
[[nodiscard]] std::int32_t achv_stage(std::int32_t achievement_id, const Account& account,
                                      const TableBlob* tables);

// 成就总点数 = 已完成 (进度 >= 目标) 的成就数。
[[nodiscard]] std::int32_t achv_total_points(const Account& account, const TableBlob* tables);

struct AchvClaimResult {
    bool ok{false};
    std::int32_t code{10}; // LogicCode::Ok = 10
    std::int32_t achv_id{};
    std::int32_t status{2};    // 领取后状态: Finish
    bool player{false};        // 钱包变动, 需推 PlayerData
    std::vector<AccountItem> bag;
    std::vector<std::pair<std::int32_t, std::int32_t>> shown; // 弹窗展示 {itemId, num}
};
[[nodiscard]] AchvClaimResult claim_achievement(Account& account, const TableBlob* tables,
                                                std::int32_t achievement_id);

// 成就点数里程碑阈值 = TaskControl(id=5).CompleteAchievementNumber (f9)。
[[nodiscard]] std::vector<std::int32_t> achv_point_thresholds(const TableBlob* tables);

struct AchvPointClaimResult {
    bool ok{false};
    std::int32_t code{10};
    std::int32_t point_id{};   // 阈值下标 (0-based)
    std::int32_t status{2};
    bool player{false};
    std::vector<AccountItem> bag;
    std::vector<std::pair<std::int32_t, std::int32_t>> shown;
};
[[nodiscard]] AchvPointClaimResult claim_achievement_point(Account& account, const TableBlob* tables,
                                                           std::int32_t point_id);

} // namespace x2::offline
