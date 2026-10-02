#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "offline/account_state.hpp"
#include "offline/table_blob.hpp"

namespace x2::offline {

struct CheatRequest {
    std::int32_t opt{-1};
    bool isset{};
    std::vector<std::int64_t> values;
    std::vector<std::string> strvals;
};

[[nodiscard]] CheatRequest parse_cheat(std::span<const std::uint8_t> body);


struct CheatEffect {
    bool supported{true};
    bool player{};                   // PlayerDataProto (pid 1000)
    std::vector<AccountHero> heroes; // L2C_HeroUpdate (549)
    std::vector<AccountItem> items;  // L2C_ItemUpdate (553)
    std::int32_t favor_hero{};
    std::int32_t favor_before_level{};
    std::int32_t favor_before_exp{};
    std::int32_t favor_after_level{};
    std::int32_t favor_after_exp{};
    std::int32_t level_before{};
    std::int32_t level_after{};
};


[[nodiscard]] CheatEffect apply_cheat(Account& account, const CheatRequest& request,
                                       const TableBlob* tables = nullptr);

struct MailClaim {
    bool ok{};
    bool player{};
    AccountItem bag{};
    std::int32_t granted{};
    std::vector<AccountItem> bag_items;
    std::vector<AccountEquip> equips;
    std::vector<AccountItem> shown;
};

[[nodiscard]] MailClaim claim_mail(Account& account, std::int64_t mail_id, const TableBlob* tables = nullptr);
[[nodiscard]] int mark_mails_read(Account& account, std::span<const std::int64_t> ids);
[[nodiscard]] int delete_mails(Account& account, std::span<const std::int64_t> ids);

} // namespace x2::offline
