#include "offline/npc_social.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <ranges>

#include "offline/game_table.hpp"
#include "proto/protobuf.hpp"
#include "server/protocol_ids.hpp"

namespace x2::offline {

namespace {

using proto::Writer;


bool section_cleared(const Account& account, std::int32_t section) {
    return std::ranges::contains(account.player.cleared_main, section);
}


[[nodiscard]] std::vector<const TableRow*> visible_blogs(const Account& account, const GameTable& blog_table) {
    std::vector<const TableRow*> out;
    for (const auto& row : blog_table.rows()) {
        const auto trigger = blog_table.int_field(row, 3).value_or(0);
        const auto type_number = blog_table.int_field(row, 4).value_or(0);
        switch (trigger) {
            case 1: {
                const auto hero_id = blog_table.int_field(row, 2).value_or(0);
                const auto h = std::ranges::find(account.heroes, hero_id, &AccountHero::id);
                if (h == account.heroes.end() || h->favor_level < type_number) continue;
                break;
            }
            case 13: {
                if (type_number < 20000000 || !section_cleared(account, type_number - 20000000)) continue;
                break;
            }
            case 5: {
                if (!section_cleared(account, type_number)) continue;
                break;
            }
            default:
                continue; // 活动类后面再说
        }
        out.push_back(&row);
    }
    return out;
}

void append_pair(Writer& w, std::uint32_t field, std::int32_t k, std::int32_t v) {
    Writer pair;
    pair.int32(1, k);
    pair.int32(2, v);
    w.message(field, pair.data());
}

} // namespace


constexpr std::int64_t kFavorTimeBase = 1514736000;
static std::vector<std::uint8_t> build_blog_box(const Account& account, const GameTable& blog,
                                                const std::vector<const TableRow*>& rows,
                                                std::int32_t hero_id) {
    Writer box;
    box.int32(1, hero_id);
    for (const auto* row : rows) {
        const auto blog_id = blog.int_field(*row, 1).value_or(0);
        Writer group;
        group.int32(1, blog_id);                                    // groupID = BlogID
        group.int32(2, -static_cast<std::int32_t>(blog_id % 1000000)); // startTime (确定性)
        if (const auto it = account.player.npc_blog_likes.find(blog_id);
            it != account.player.npc_blog_likes.end() && it->second > 0) {
            group.int32(3, it->second);                             // likeTime (偏移值)
        }
        if (const auto it = account.player.npc_blog_replies.find(blog_id);
            it != account.player.npc_blog_replies.end() && !it->second.empty()) {
            Writer chat_group;
            chat_group.int32(1, it->second.front().chat_group_id);
            for (const auto& r : it->second) {
                append_pair(chat_group, 2, r.reply_id, r.time_offset);
            }
            group.message(4, chat_group.data());
        }
        box.message(2, group.data());
    }
    return box.data();
}

namespace {

[[nodiscard]] std::set<std::int32_t> active_hero_set(const Account& account) {
    std::set<std::int32_t> out;
    for (const auto& h : account.heroes) {
        if (h.state == 2 && (h.favor_level > 0 || h.favor_exp > 0 || h.id == 1003)) {
            out.insert(h.id);
        }
    }
    return out;
}

} // namespace

std::vector<std::uint8_t> query_npc_blog(const Account& account, const TableBlob* tables, std::int64_t now_unix) {
    Writer body;
    body.int32(1, static_cast<std::int32_t>(server::LogicCode::Ok)); // 1: code

    if (!tables) return body.data();
    GameTable blog;
    if (!blog.load(*tables, "FavorabilityBlog")) return body.data();

    const auto blogs = visible_blogs(account, blog);
    if (blogs.empty()) {
        const auto h = std::ranges::find(account.heroes, 1003, &AccountHero::id);
        std::fprintf(stderr,
                     "[npc_blog][EMPTY] heroes=%zu h1003_favor=%d cleared=%zu\n",
                     account.heroes.size(),
                     h != account.heroes.end() ? h->favor_level : -1,
                     static_cast<unsigned long>(account.player.cleared_main.size()));
    }

    const auto active_heroes = active_hero_set(account);
    std::map<std::int32_t, std::vector<const TableRow*>> by_hero;
    for (const auto* row : blogs) {
        const auto hero = blog.int_field(*row, 2).value_or(0);
        if (hero > 0 && active_heroes.contains(hero)) by_hero[hero].push_back(row);
    }

    for (const auto& [hero_id, rows] : by_hero) {
        const auto box = build_blog_box(account, blog, rows, hero_id);
        body.message(2, std::span{box.data(), box.size()}); // 2: repeated blogBox
    }
    return body.data();
}

static std::vector<std::uint8_t> build_letter_box(const Account& account, const TableBlob* tables,
                                                  std::int32_t hero_id, std::int64_t now_unix) {
    Writer box;
    box.int32(1, hero_id);

    GameTable mail;
    if (!tables || !mail.load(*tables, "PrivateMail")) return box.data();

    const auto h = std::ranges::find(account.heroes, hero_id, &AccountHero::id);
    const auto favor_level = h != account.heroes.end() ? h->favor_level : 0;

    std::map<std::int32_t, std::vector<const TableRow*>> groups;
    for (const auto& row : mail.rows()) {
        if (mail.int_field(row, 2).value_or(0) != hero_id) continue; // f2 HeroID
        groups[mail.int_field(row, 3).value_or(0)].push_back(&row); // f3 GroupID
    }

    for (auto& [group_id, rows] : groups) {
        const auto* head = rows.front();
        const auto trigger = mail.int_field(*head, 4).value_or(0);      // f4 TriggerType
        const auto type_number = mail.int_field(*head, 5).value_or(0);  // f5 TypeNumber
        switch (trigger) {
            case 1:
                if (favor_level < type_number) continue;
                break;
            case 13:
                if (type_number < 20000000 || !section_cleared(account, type_number - 20000000)) continue;
                break;
            case 5:
                if (!section_cleared(account, type_number)) continue;
                break;
            case 14: // 仅 2 行, 条件未知, 暂放行
            default:
                break;
        }

        Writer group;
        group.int32(1, static_cast<std::int32_t>(now_unix - kFavorTimeBase - 86400)); // 1: startTime
        for (std::size_t i = 0; i < rows.size(); ++i) {
            Writer letter;                                              // repeated letterData
            letter.int32(1, mail.int_field(*rows[i], 1).value_or(0));   // letterID = PrivateMailID
            letter.int32(2, 0);                                         // replyID 
            letter.int32(3, static_cast<std::int32_t>(i));              // index (组内排序)
            group.message(2, letter.data());
        }
        group.int32(3, 1);                                              // status = 已收到可读
        box.message(2, group.data());
    }

    box.int32(3, 0);                                                    // lastReplyTime (无待解锁信)
    return box.data();
}

std::vector<std::uint8_t> query_npc_letter(const Account& account, const TableBlob* tables, std::int64_t now_unix) {
    Writer body;
    body.int32(1, static_cast<std::int32_t>(server::LogicCode::Ok)); // 1: code
    GameTable control;
    if (!tables || !control.load(*tables, "PrivateMailControl")) return body.data();
    const auto active = active_hero_set(account);
    for (const auto& row : control.rows()) {
        const auto hero = static_cast<std::int32_t>(row.key);
        if (!active.contains(hero)) continue;
        body.message(2, build_letter_box(account, tables, hero, now_unix));
    }

    return body.data();
}

std::vector<std::uint8_t> update_private_letter(const Account& account, const TableBlob* tables,
                                                std::int32_t hero_id, std::int64_t now_unix) {
    Writer body;
    body.int32(1, static_cast<std::int32_t>(server::LogicCode::Ok));
    body.message(2, build_letter_box(account, tables, hero_id, now_unix));
    return body.data();
}

std::vector<std::uint8_t> like_npc_blog(Account& account, const TableBlob* tables, std::int32_t hero_id,
                                        std::int32_t group_id, std::int64_t now_unix) {
    Writer body;
    body.int32(1, static_cast<std::int32_t>(server::LogicCode::Ok));
    if (!tables) return body.data();
    GameTable blog;
    if (!blog.load(*tables, "FavorabilityBlog")) return body.data();

    auto& likes = account.player.npc_blog_likes;
    if (group_id > 0) {
        if (likes.contains(group_id)) {
            likes.erase(group_id);
        } else {
            likes.emplace(group_id, static_cast<std::int32_t>(now_unix - kFavorTimeBase));
        }
    }

    std::vector<const TableRow*> rows;
    for (const auto& row : blog.rows()) {
        if (blog.int_field(row, 2).value_or(0) == hero_id) rows.push_back(&row);
    }
    const auto box = build_blog_box(account, blog, rows, hero_id);
    body.message(2, std::span{box.data(), box.size()}); // 2: blogBox
    return body.data();
}

std::vector<std::uint8_t> reply_npc_blog(Account& account, const TableBlob* tables, std::int32_t hero_id,
                                         std::int32_t group_id, std::int32_t chat_group_id, std::int32_t reply_id,
                                         std::int64_t now_unix) {
    Writer body;
    body.int32(1, static_cast<std::int32_t>(server::LogicCode::Ok));
    if (!tables) return body.data();
    GameTable blog;
    if (!blog.load(*tables, "FavorabilityBlog")) return body.data();

    if (group_id > 0 && reply_id > 0) {
        auto& replies = account.player.npc_blog_replies[group_id];
        const auto dup = std::ranges::any_of(replies, [reply_id](const auto& r) {
            return r.reply_id == reply_id;
        });
        if (!dup) {
            replies.push_back({.reply_id = reply_id,
                               .time_offset = static_cast<std::int32_t>(now_unix - kFavorTimeBase),
                               .chat_group_id = chat_group_id});
        }
    }

    std::vector<const TableRow*> rows;
    for (const auto& row : blog.rows()) {
        if (blog.int_field(row, 2).value_or(0) == hero_id) rows.push_back(&row);
    }
    const auto box = build_blog_box(account, blog, rows, hero_id);
    body.message(2, std::span{box.data(), box.size()}); // 2: blogBox
    return body.data();
}

} // namespace x2::offline
