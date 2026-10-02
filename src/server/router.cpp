#include "server/router.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <ctime>
#include <format>
#include <map>
#include <print>
#include <utility>

#include "core/log.hpp"
#include "offline/cheat.hpp"
#include "offline/game_table.hpp"
#include "offline/achievement.hpp"
#include "offline/mission.hpp"
#include "offline/npc_social.hpp"
#include "offline/protocol_builders.hpp"
#include "offline/shop.hpp"
#include "proto/protobuf.hpp"
#include "server/protocol_ids.hpp"

namespace x2::server {

namespace {
constexpr std::string_view tag{"ROUTER"};

std::string now_token() {
    return std::format("offline_{}", static_cast<std::int64_t>(std::time(nullptr)));
}

HttpResponse json(std::string body) {
    return HttpResponse{.status = 200, .content_type = "application/json", .body = std::move(body)};
}

std::int32_t current_time() {
    return static_cast<std::int32_t>(std::time(nullptr));
}

int mail_state_filter(std::string_view body) {
    constexpr std::string_view key{"\"state\":"};
    const auto pos = body.find(key);
    if (pos == std::string_view::npos) return -1;
    std::size_t i = pos + key.size();
    int sign = 1;
    if (i < body.size() && body[i] == '-') {
        sign = -1;
        ++i;
    }
    int value = 0;
    while (i < body.size() && body[i] >= '0' && body[i] <= '9') value = value * 10 + (body[i++] - '0');
    return sign * value;
}

std::string json_escape(std::string_view raw) {
    std::string out;
    for (const char c : raw) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

std::string mail_object_json(const offline::AccountMail& mail) {
    const auto attachment = json_escape(
        std::format(R"({{"attachment":[{{"item":{},"num":{}}}]}})", mail.item_id, mail.num));
    return std::format(
        R"({{"id":{},"from":"GM","title":"GM","body":"","state":{},"time":{},"attachment":"{}","type":0,"expire_at":{},"operate_id":"","read_time":0,"recv_time":0,"del_time":0}})",
        mail.id, mail.state, mail.time, attachment, mail.time + 30LL * 86400);
}

std::string mail_page_json(const offline::Account& account, std::string_view body) {
    const int want = mail_state_filter(body);
    std::string mails;
    int total = 0;
    for (const auto& mail : account.player.mails) {
        if (mail.state == 4) continue;
        if (want >= 0 && mail.state != want) continue;
        if (!mails.empty()) mails += ',';
        mails += mail_object_json(mail);
        ++total;
    }
    return std::format(R"({{"total":{},"mails":[{}]}})", total, mails);
}

std::vector<std::int64_t> json_id_list(std::string_view body) {
    std::vector<std::int64_t> ids;
    const auto key = body.find("\"ids\"");
    if (key == std::string_view::npos) return ids;
    const auto begin = body.find('[', key);
    const auto end = body.find(']', begin == std::string_view::npos ? key : begin);
    if (begin == std::string_view::npos || end == std::string_view::npos) return ids;
    std::size_t i = begin + 1;
    while (i < end) {
        if (body[i] >= '0' && body[i] <= '9') {
            std::int64_t value = 0;
            while (i < end && body[i] >= '0' && body[i] <= '9') value = value * 10 + (body[i++] - '0');
            ids.push_back(value);
        } else {
            ++i;
        }
    }
    return ids;
}

std::string draw_history_json(const offline::Account& account, std::string_view body) {
    std::string pairs;
    const auto key = body.find("\"keys\"");
    const auto begin = body.find('[', key == std::string_view::npos ? body.size() : key);
    const auto end = body.find(']', begin == std::string_view::npos ? body.size() : begin);
    for (auto i = begin; end != std::string_view::npos && i < end;) {
        const auto open = body.find('"', i);
        const auto close = body.find('"', open + 1);
        if (open >= end || close >= end) break;
        i = close + 1;
        const auto name = body.substr(open + 1, close - open - 1);
        const auto cut = name.find('_');
        std::int32_t pool_id = 0, n = 0;
        if (cut == std::string_view::npos ||
            std::from_chars(name.data(), name.data() + cut, pool_id).ec != std::errc{} ||
            std::from_chars(name.data() + cut + 1, name.data() + name.size(), n).ec != std::errc{})
            continue;
        const auto pool = account.player.draw_pools.find(pool_id);
        if (pool == account.player.draw_pools.end()) continue;
        const auto& history = pool->second.history;
        const auto back = static_cast<std::int64_t>(pool->second.one) + 10LL * pool->second.ten - n;
        if (n <= 0 || back < 0 || back >= std::ssize(history)) continue;
        const auto& record = history[history.size() - 1 - static_cast<std::size_t>(back)];
        const auto val = std::format(R"({{"extra":false,"time":{},"items":[{{"num":{},"id":{},"isTrans":{}}}]}})",
                                     offline::client_time(record.time), record.num, record.item_id, record.trans);
        if (!pairs.empty()) pairs += ',';
        pairs += std::format(R"({{"key":"{}","val":"{}"}})", name, json_escape(val));
    }
    return std::format(R"({{"pairs":[{}]}})", pairs);
}

std::string mails_json(const offline::Account& account, const std::vector<std::int64_t>& ids) {
    std::string mails;
    for (const auto id : ids) {
        const auto mail = std::ranges::find(account.player.mails, id, &offline::AccountMail::id);
        if (mail == account.player.mails.end()) continue;
        if (!mails.empty()) mails += ',';
        mails += mail_object_json(*mail);
    }
    return std::format(R"({{"mails":[{}]}})", mails);
}

std::vector<std::pair<std::string, std::string>> parse_form(std::string_view body) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string_view pair;
    const auto push = [&out](std::string_view p) {
        const auto eq = p.find('=');
        if (eq == std::string_view::npos) {
            out.emplace_back(std::string{p}, "");
        } else {
            out.emplace_back(std::string{p.substr(0, eq)}, std::string{p.substr(eq + 1)});
        }
    };
    std::size_t pos = 0;
    while (pos < body.size()) {
        const auto amp = body.find('&', pos);
        if (amp == std::string_view::npos) {
            push(body.substr(pos));
            break;
        }
        push(body.substr(pos, amp - pos));
        pos = amp + 1;
    }
    return out;
}

struct LoginRequest {
    std::int64_t id{};
    std::string token;
    std::string device_id;
    std::int32_t connect_type{};
};

LoginRequest parse_login(const std::span<const std::uint8_t> body) {
    LoginRequest result;
    proto::Reader reader{body};
    while (!reader.eof()) {
        const auto field = reader.field();
        if (!field) break;
        if (field->number == 1 && field->wire_type == 0) {
            if (const auto value = reader.varint()) result.id = static_cast<std::int64_t>(*value);
        } else if (field->number == 2 && field->wire_type == 2) {
            if (const auto value = reader.string()) result.token = *value;
        } else if (field->number == 3 && field->wire_type == 2) {
            if (const auto value = reader.string()) result.device_id = *value;
        } else if (field->number == 4 && field->wire_type == 0) {
            if (const auto value = reader.varint()) result.connect_type = static_cast<std::int32_t>(*value);
        } else if (!reader.skip(field->wire_type)) {
            break;
        }
    }
    return result;
}

}

void Router::set_tables(std::shared_ptr<offline::TableBlob> tables) {
    tables_ = tables;
    accounts_.set_tables(tables_);
}

offline::Account& Router::account_for_login(const marsnet::Frame& frame) {
    const auto request = parse_login(frame.body);
    // Offline single player: one save per device. The token changes on every
    // login (/apply/httpLogin mints a fresh one), so it cannot key the save.
    account_key_ = request.device_id.empty() ? std::string{"anonymous"} : request.device_id;
    account_ = accounts_.create_or_get(account_key_, request.token);
    return account_;
}

std::vector<std::uint8_t> Router::push_growth(const marsnet::Frame& frame) {
    return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_QueryGrowthBase), 0, frame.head.session_id,
                                    ++data_version_, static_cast<std::int32_t>(LogicCode::Ok), "",
                                    offline::ProtocolBuilders::growth_base(account_, tables_.get()));
}

std::vector<std::uint8_t> Router::push_item_updates(const marsnet::Frame& frame,
                                                    const std::vector<offline::AccountItem>& items) {
    if (items.empty()) return {};
    proto::Writer update; // L2C_ItemUpdate: 1 code, 2 repeated ItemData
    update.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
    for (const auto& item : items) update.message(2, offline::ProtocolBuilders::item_data(item));
    return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_ItemUpdate), 0,
                                    frame.head.session_id, ++data_version_,
                                    static_cast<std::int32_t>(LogicCode::Ok), "", update.data());
}

std::vector<std::uint8_t> Router::college_done_pushes(const marsnet::Frame& frame, std::int32_t building_id) {
    std::vector<std::uint8_t> out;
    proto::Writer up; // L2C_UpLevelBuildingId: 1 buildingId
    up.int32(1, building_id);
    auto push = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_UpLevelBuildingId), 0,
                                         frame.head.session_id, ++data_version_,
                                         static_cast<std::int32_t>(LogicCode::Ok), "", up.data());
    out.insert(out.end(), push.begin(), push.end());
    push = push_growth(frame);
    out.insert(out.end(), push.begin(), push.end());
    return out;
}

std::vector<std::uint8_t> Router::push_card_pool(const marsnet::Frame& frame) {
    if (!tables_) return {};
    return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_CardPool), 0, frame.head.session_id,
                                    ++data_version_, static_cast<std::int32_t>(LogicCode::Ok), "",
                                    offline::ProtocolBuilders::card_pool(*tables_, account_, current_time()));
}

std::vector<std::uint8_t> Router::push_player_data(const marsnet::Frame& frame) {
    if (account_.player.level >= 7 && account_.player.sign_start == 0) account_.player.sign_start = current_time();
    const auto body = offline::ProtocolBuilders::player_data(account_, current_time(), tables_.get());
    x2::core::log_line(x2::core::LogLevel::Info, tag,
                       std::format("push player_data body={}B relics={} medals={}", body.size(),
                                   account_.player.relic_pack.size(), account_.player.medal_pack.size()));
    return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::PlayerDataProto), 0, frame.head.session_id,
                                    ++data_version_, static_cast<std::int32_t>(LogicCode::Ok), "", body);
}

std::vector<std::uint8_t> Router::push_level_up(const marsnet::Frame& frame, std::int32_t before, std::int32_t after) {
    if (after <= before) return {};
    proto::Writer body; // L2C_UpdatePlayerLevel: 1 beforeLevel, 2 afterLevel
    body.int32(1, before);
    body.int32(2, after);
    return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_UpdatePlayerLevel), 0, frame.head.session_id,
                                    ++data_version_, static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
}

HttpResponse Router::http(const HttpRequest& request) {
    if (request.path.starts_with("/banner/")) {
        static constexpr unsigned char kMinimalPng[] = {
            0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
            0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
            0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
            0x0a, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x00, 0x01, 0x00, 0x00,
            0x05, 0x00, 0x01, 0x0d, 0x0a, 0x2d, 0xb4, 0x00, 0x00, 0x00, 0x00, 0x49,
            0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
        };
        HttpResponse resp;
        resp.status = 200;
        resp.content_type = "image/png";
        resp.body = std::string(reinterpret_cast<const char*>(kMinimalPng), sizeof(kMinimalPng));
        return resp;
    }
    if (request.path == "/apply/httpLogin" || request.path == "/apply/httpLogin163") {
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("httpLogin body={}", request.body.substr(0, 512)));

        return json(std::format(
            R"({{"code":1,"token":"{}","playerID":1,"entryIP":"127.0.0.1","entryPort":"10001","logicCode":0,"serverID":"1"}})",
            now_token()));
    }
    if (request.path == "/loginwithpw" || request.path == "/apply/loginwithpw") {
        const std::string token = now_token();
        std::string account;
        for (const auto& pair : parse_form(request.body)) {
            if (pair.first == "account" || pair.first == "username") account = pair.second;
        }
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("loginwithpw account={}", account));
        const auto now = current_time();
        return json(std::format(
            R"({{"code":"ok","token":"{}","id":1,"start":{},"expire":{}}})",
            token, now, now + 86400));
    }
    if (request.path == "/apply/controlInfo") {
        return json(R"({"giftCode":"","update":"LEBIAN"})");
    }
    if (request.path == "/apply/connectInfo") {
        return json(
            R"({"result":{"serviceAppId":"1")"
            R"(,"pbsServer":"http://127.0.0.1:9999")"
            R"(,"loginServer":"http://127.0.0.1:9999")"
            R"(,"accountServer":"http://127.0.0.1:9999")"
            R"(,"eswebServer":"http://127.0.0.1:9999")"
            R"(,"lbPbsServer":["http://127.0.0.1:9999"])"
            R"(,"lbLoginServer":["http://127.0.0.1:9999"])"
            R"(,"lbEswebServer":["http://127.0.0.1:9999"])"
            R"(,"areaId":"1"}})");
    }
    if (request.path == "/apply/address") {
        return json(
            R"({"result":{"data":[{"ip":"127.0.0.1","port":10001,"weight":1,"desc":"offline","zid":1}]}})");
    }
    if (request.path == "/register") {
        return json(R"({"success":true})");
    }
    if (request.path == "/apply/chatNode") {
        return json(R"([{"channel":"[{\"channel\":1,\"free\":100,\"limit\":100}]","entry":"127.0.0.1:10001","nodeType":"_node.chat","token":"offline_chat_token"}])");
    }
    if (request.path == "/CometService.GetEndpoint") {
        x2::core::log_line(x2::core::LogLevel::Info, tag, std::format("pbs {} body: {}", request.path, request.body));
        std::string host = "127.0.0.1:9999";
        for (const auto& [name, val] : request.headers) {
            if (name == "Host" && !val.empty()) {
                host = val;
                break;
            }
        }
        return json(std::format(R"({{"error":"","code":0,"data":{{"endpoint":"ws://{}/ws"}}}})", host));
    }
    if (request.path == "/MailService.GetMailPage" || request.path == "/MailService.GetMailPageV2") {
        x2::core::log_line(x2::core::LogLevel::Info, tag, std::format("pbs {} body: {}", request.path, request.body));
        return json(std::format(R"({{"error":"","code":0,"data":{}}})", mail_page_json(account_, request.body)));
    }
    if (request.path == "/MailService.GetMailList") {
        x2::core::log_line(x2::core::LogLevel::Info, tag, std::format("pbs {} body: {}", request.path, request.body));
        const auto ids = json_id_list(request.body);
        std::string mails;
        if (!ids.empty()) {
            for (const auto id : ids) {
                const auto mail = std::ranges::find(account_.player.mails, id, &offline::AccountMail::id);
                if (mail == account_.player.mails.end()) continue;
                if (!mails.empty()) mails += ',';
                mails += mail_object_json(*mail);
            }
        } else {
            for (const auto& mail : account_.player.mails) {
                if (mail.state == 4) continue;
                if (!mails.empty()) mails += ',';
                mails += mail_object_json(mail);
            }
        }
        return json(std::format(R"({{"error":"","code":0,"data":{{"mail_list":[{}]}}}})", mails));
    }
    if (request.path == "/MailService.GetMail") {
        x2::core::log_line(x2::core::LogLevel::Info, tag, std::format("pbs {} body: {}", request.path, request.body));
        std::int64_t target_id = 0;
        const auto key = request.body.find("\"id\":");
        if (key != std::string::npos) {
            std::size_t i = key + 5;
            while (i < request.body.size() && (request.body[i] == ' ' || request.body[i] == ':')) ++i;
            while (i < request.body.size() && request.body[i] >= '0' && request.body[i] <= '9') {
                target_id = target_id * 10 + (request.body[i++] - '0');
            }
        }
        const auto mail = std::ranges::find(account_.player.mails, target_id, &offline::AccountMail::id);
        std::string mail_json = mail != account_.player.mails.end() ? mail_object_json(*mail) : "{}";
        return json(std::format(R"({{"error":"","code":0,"data":{{"mail":{}}}}})", mail_json));
    }
    if (request.path == "/MailService.CountMails") {
        int count = 0;
        for (const auto& m : account_.player.mails) {
            if (m.state != 4) ++count;
        }
        return json(std::format(R"({{"error":"","code":0,"data":{{"total":{}}}}})", count));
    }
    if (request.path == "/KVStoreService.GetValues") {
        x2::core::log_line(x2::core::LogLevel::Info, tag, std::format("pbs {} body: {}", request.path, request.body));
        return json(std::format(R"({{"error":"","code":0,"data":{}}})", draw_history_json(account_, request.body)));
    }
    if (request.path == "/MailService.MarkRead") {
        const auto ids = json_id_list(request.body);
        if (offline::mark_mails_read(account_, ids) > 0) accounts_.save(account_key_, account_);
        x2::core::log_line(x2::core::LogLevel::Info, tag, std::format("pbs {} body: {}", request.path, request.body));
        return json(std::format(R"({{"error":"","code":0,"data":{}}})", mails_json(account_, ids)));
    }
    if (request.path == "/MailService.DelMail" || request.path == "/MailService.RemoveReadMails") {
        const auto ids = request.path == "/MailService.RemoveReadMails"
                             ? [&] {
                                   std::vector<std::int64_t> read;
                                   for (const auto& mail : account_.player.mails) {
                                       if (mail.state == 1 || mail.state == 3) read.push_back(mail.id);
                                   }
                                   return read;
                               }()
                             : json_id_list(request.body);
        const int removed = offline::delete_mails(account_, ids);
        if (removed > 0) accounts_.save(account_key_, account_);
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("pbs {} removed={} body: {}", request.path, removed, request.body));
        return json(std::format(R"({{"error":"","code":0,"data":{{"removed":{}}}}})", removed));
    }
    struct PbsReply {
        std::string_view path;
        std::string_view data;
    };
    static constexpr PbsReply kPbsReplies[] = {
#include "server/pbs_replies.inc"
    };
    if (const auto* reply = std::ranges::find(kPbsReplies, request.path, &PbsReply::path);
        reply != std::end(kPbsReplies)) {
        x2::core::log_line(x2::core::LogLevel::Info, tag, std::format("pbs {} body: {}", request.path, request.body));
        const std::string_view data = request.path == "/ClubService.GetClub" ? "null" : reply->data;
        return json(std::format(R"({{"error":"","code":0,"data":{}}})", data));
    }
    std::string headers;
    for (const auto& [key, value] : request.headers) headers += std::format("{}={}; ", key, value);
    x2::core::log_line(x2::core::LogLevel::Warn, tag,
                       std::format("unhandled http {} headers: {} body: {}", request.path, headers,
                                   request.body.substr(0, 512)));
    return json(R"({"code":0})");
}

std::vector<std::uint8_t> Router::poll_pushes() {
    if (account_key_.empty() || account_.id <= 0) return {};
    const auto now = static_cast<std::int32_t>(std::time(nullptr));
    const auto done = offline::finish_expired_college_build(account_, now);
    if (!done) return {};
    accounts_.save(account_key_, account_);
    x2::core::log_line(x2::core::LogLevel::Info, tag,
                       std::format("college build completed id={} (timer)", done));
    const marsnet::Frame frame{.head = {.session_id = last_session_}};
    return college_done_pushes(frame, done);
}

std::vector<std::uint8_t> Router::marsnet(const marsnet::Frame& frame) {
    last_session_ = frame.head.session_id;
    if (frame.head.ack_data_version > 0) {
        data_version_ = std::max(data_version_, frame.head.ack_data_version);
    }
    x2::core::log_line(x2::core::LogLevel::Info, tag,
                       std::format("proto=0x{:04x} request={} ack_ver={} body={} bytes",
                                   frame.proto_id, frame.head.request_id, frame.head.ack_data_version,
                                   frame.body.size()));

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_Login)) {
        auto& account = account_for_login(frame);
        if (tables_) {
            bool dirty = offline::drop_maze_items(account, *tables_);
            const auto task_day = account.player.task_day;
            offline::roll_daily_tasks(account, *tables_, std::time(nullptr));
            dirty = dirty || task_day != account.player.task_day;
            if (offline::migrate_bag_medals(account, tables_.get(), std::time(nullptr))) dirty = true;
            if (account.player.guides.empty()) {
                offline::seed_guides(account, *tables_);
                dirty = true;
            }
            for (auto& hero : account.heroes) {
                const auto new_dubbings = offline::dubbing_unlock_ids(hero, tables_.get());
                if (new_dubbings != hero.dubbings) {
                    hero.dubbings = new_dubbings;
                    dirty = true;
                }
            }
            if (dirty) accounts_.save(account_key_, account);
        }
        std::int32_t next_section = 0;
        if (tables_ && account.player.main_section > 0) {
            offline::GameTable section;
            if (section.load(*tables_, "SectionTable")) {
                if (const auto row = section.row(static_cast<std::uint64_t>(account.player.main_section))) {
                    next_section = section.int_field(*row, 2).value_or(0);
                }
            }
        }
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("login key={} section={} next={} cleared={} guides={} pending={}", account_key_,
                                       account.player.main_section, next_section, account.player.cleared_main.size(),
                                       account.player.guides.size(), account.player.pending_section));
        data_version_ = 0;
        auto out = push_player_data(frame);
        auto login = offline::ProtocolBuilders::login(account, current_time(), tables_.get());
        if (tables_) {
            if (const auto profile = offline::ProtocolBuilders::fight_profile(tables_.get(), account); !profile.empty()) {
                proto::Writer extra;
                extra.message(4, profile); // L2C_Login.fightDataProfile
                login.insert(login.end(), extra.data().begin(), extra.data().end());
            }
        }
        const auto reply = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_Login),
                                                    frame.head.request_id, frame.head.session_id, 2,
                                                    static_cast<std::int32_t>(LogicCode::Ok), "", login);
        out.insert(out.end(), reply.begin(), reply.end());
        const auto pools = push_card_pool(frame);
        out.insert(out.end(), pools.begin(), pools.end());
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_SelectRole)) {
        std::int32_t role = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->number == 1 && field->wire_type == 0) {
                if (const auto value = reader.varint()) role = static_cast<std::int32_t>(*value);
            } else if (!reader.skip(field->wire_type)) {
                break;
            }
        }
        x2::core::log_line(x2::core::LogLevel::Info, tag, std::format("select role {}", role));
        proto::Writer body;
        body.int64(1, 1);
        body.int32(2, role);
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_SelectRole),
                                         frame.head.request_id, frame.head.session_id, 1,
                                         static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_ReConnect)) {
        if (frame.head.ack_data_version > 0) {
            data_version_ = frame.head.ack_data_version;
        }
        auto& account = account_for_login(frame);
        if (tables_) {
            const auto seeded = account.player.guides.empty();
            if (seeded) offline::seed_guides(account, *tables_);
            bool dirty = seeded || offline::fix_worn_slots(account, *tables_);
            for (auto& hero : account.heroes) {
                const auto new_dubbings = offline::dubbing_unlock_ids(hero, tables_.get());
                if (new_dubbings != hero.dubbings) {
                    hero.dubbings = new_dubbings;
                    dirty = true;
                }
            }
            if (dirty) accounts_.save(account_key_, account);
        }
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("reconnect key={} section={} cleared={} pending={}", account_key_,
                                       account.player.main_section, account.player.cleared_main.size(),
                                       account.player.pending_section));
        auto out = push_player_data(frame);
        {
            const auto growth = push_growth(frame);
            out.insert(out.end(), growth.begin(), growth.end());
            const auto pools = push_card_pool(frame);
            out.insert(out.end(), pools.begin(), pools.end());
        }
        {
            proto::Writer body;
            for (const auto& hero : account.heroes)
                body.message(1, offline::ProtocolBuilders::hero_data(hero, tables_.get()));
            const auto push = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_HeroAll), 0,
                                                       frame.head.session_id, ++data_version_,
                                                       static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
            out.insert(out.end(), push.begin(), push.end());
        }
        {
            const auto skin_push = marsnet::encode_response(
                static_cast<std::uint32_t>(ProtoId::L2C_HeroSkinAll), 0,
                frame.head.session_id, ++data_version_,
                static_cast<std::int32_t>(LogicCode::Ok), "",
                offline::build_hero_skin_all(account, tables_.get()));
            out.insert(out.end(), skin_push.begin(), skin_push.end());
        }
        auto reconnected = account;
        reconnected.is_create_role = false;
        auto body = offline::ProtocolBuilders::reconnect(reconnected, current_time());
        if (const auto profile = offline::ProtocolBuilders::fight_profile(tables_.get(), account); !profile.empty()) {
            proto::Writer extra;
            extra.message(3, profile); // L2C_ReConnect.fightDataProfile
            body.insert(body.end(), extra.data().begin(), extra.data().end());
        }
        const auto reply = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_ReConnect),
                                                    frame.head.request_id, frame.head.session_id, data_version_,
                                                    static_cast<std::int32_t>(LogicCode::Ok), "",
                                                    body);
        out.insert(out.end(), reply.begin(), reply.end());
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_Cheat)) {
        const auto request = offline::parse_cheat(frame.body);
        const auto effect = offline::apply_cheat(account_, request, tables_.get());
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("cheat opt={} {} body: {}", request.opt, effect.supported ? "ok" : "unsupported",
                                       proto::dump(frame.body)));
        std::vector<std::uint8_t> out;
        const auto append = [&out](const std::vector<std::uint8_t>& bytes) {
            out.insert(out.end(), bytes.begin(), bytes.end());
        };
        // L2C_HeroUpdate / L2C_ItemUpdate: {f1 code, f2 repeated HeroData/ItemData}.
        const auto push_update = [&](ProtoId pid, const std::vector<std::vector<std::uint8_t>>& entries) {
            proto::Writer body;
            body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
            for (const auto& entry : entries) body.message(2, entry);
            append(marsnet::encode_response(static_cast<std::uint32_t>(pid), 0, frame.head.session_id,
                                            ++data_version_, static_cast<std::int32_t>(LogicCode::Ok), "",
                                            body.data()));
        };
        LogicCode code = LogicCode::Ok;
        if (effect.supported) {
            accounts_.save(account_key_, account_);
            if (effect.player) append(push_player_data(frame));
            if (!effect.heroes.empty()) {
                std::vector<std::vector<std::uint8_t>> entries;
                for (const auto& hero : effect.heroes)
                    entries.push_back(offline::ProtocolBuilders::hero_data(hero, tables_.get()));
                push_update(ProtoId::L2C_HeroUpdate, entries);
                // GM 解锁/升星跨 5 星阈值 → 推单英雄外观全量 (E_Stage 觉醒皮肤+星级头像)
                if (request.opt == 4 || request.opt == 7) {   // CO_UNLOCK_HERO / CO_HERO_UPSTAR
                    for (const auto& hero : effect.heroes) {
                        append(marsnet::encode_response(
                            static_cast<std::uint32_t>(ProtoId::L2C_HeroSkinUpdate), 0,
                            frame.head.session_id, ++data_version_,
                            static_cast<std::int32_t>(LogicCode::Ok), "",
                            offline::build_hero_skin_update(hero.id, account_, tables_.get())));
                    }
                }
            }
            if (!effect.items.empty()) {
                std::vector<std::vector<std::uint8_t>> entries;
                for (const auto& item : effect.items) entries.push_back(offline::ProtocolBuilders::item_data(item));
                push_update(ProtoId::L2C_ItemUpdate, entries);
            }
            if (effect.level_after > effect.level_before) {
                append(push_level_up(frame, effect.level_before, effect.level_after));
            }
            if (effect.favor_hero > 0) {
                proto::Writer info;
                info.int32(1, effect.favor_before_level);
                info.int32(2, effect.favor_before_exp);
                info.int32(3, effect.favor_after_level);
                info.int32(4, effect.favor_after_exp);
                info.int32(5, effect.favor_hero);
                info.int32(6, 2);
                proto::Writer favor;
                favor.message(1, info.data());
                append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_FavorChangeInfo), 0,
                                                frame.head.session_id, ++data_version_,
                                                static_cast<std::int32_t>(LogicCode::Ok), "", favor.data()));
            }
        } else {
            code = LogicCode::ErrorOpt;
        }
        proto::Writer body;
        body.int32(1, static_cast<std::int32_t>(code));
        body.int32(2, request.opt);
        append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_Cheat), frame.head.request_id,
                                        frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data()));
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_FightData)) {
        std::vector<std::int32_t> hero_ids;
        std::int32_t mission = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->number == 1 && field->wire_type == 2) {
                proto::Reader hero{reader.bytes().value_or(std::span<const std::uint8_t>{})};
                while (!hero.eof()) {
                    const auto f = hero.field();
                    if (!f) break;
                    if (f->number == 1 && f->wire_type == 0) hero_ids.push_back(static_cast<std::int32_t>(hero.varint().value_or(0)));
                    else if (!hero.skip(f->wire_type)) break;
                }
            } else if (field->number == 2 && field->wire_type == 0) {
                mission = static_cast<std::int32_t>(reader.varint().value_or(0));
            } else if (!reader.skip(field->wire_type)) {
                break;
            }
        }
        std::vector<std::uint8_t> out;
        account_.player.pending_section = mission;
        if (tables_) {
            offline::GameTable section;
            if (section.load(*tables_, "SectionTable")) {
                if (const auto row = section.row(static_cast<std::uint64_t>(mission))) {
                    account_.player.pending_chapter = section.int_field(*row, 7).value_or(0);
                }
            }
        }
        const auto cost = tables_ ? offline::section_power_cost(*tables_, mission) : 0;
        if (cost > 0) account_.player.power = std::max(0, account_.player.power - cost);
        accounts_.save(account_key_, account_);
        if (cost > 0) out = push_player_data(frame);
        proto::Writer body; // L2C_FightData: 1 result, 2 uuid, 4 data, 19 playerLevel
        body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
        body.string(2, now_token());
        body.bytes(4, offline::ProtocolBuilders::fight_data(tables_.get(), account_, hero_ids, mission));
        body.int32(19, account_.player.level);
        const auto reply = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_FightData),
                                                    frame.head.request_id, frame.head.session_id, data_version_,
                                                    static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
        out.insert(out.end(), reply.begin(), reply.end());
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_Account)) {
        std::int32_t opt = 0;
        std::vector<std::int32_t> values;
        std::string first_str;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->number == 1 && field->wire_type == 0) opt = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->number == 2 && field->wire_type == 0) values.push_back(static_cast<std::int32_t>(reader.varint().value_or(0)));
            else if (field->number == 3 && field->wire_type == 2 && first_str.empty()) first_str = reader.string().value_or("");
            else if (!reader.skip(field->wire_type)) break;
        }
        std::vector<std::uint8_t> out;
        constexpr std::int32_t kNickname = 0, kShow = 1, kIcon = 2, kFinishTask = 3, kNicknameOnCreate = 7;
        if ((opt == kNickname || opt == kNicknameOnCreate) && !first_str.empty()) {
            account_.player.nickname = first_str;
            accounts_.save(account_key_, account_);
            out = push_player_data(frame);
        } else if (opt == kShow && !values.empty()) {
            if (values[0] > 0) {
                account_.player.show_hero = values[0];
                accounts_.save(account_key_, account_);
                x2::core::log_line(x2::core::LogLevel::Info, tag,
                                   std::format("account show hero={} body: {}", values[0], proto::dump(frame.body)));
            }
            out = push_player_data(frame);
        } else if (opt == kIcon && !values.empty()) {
            const std::int32_t icon_id = values.size() >= 2 ? values[1] : values[0];
            if (icon_id > 0) {
                account_.player.icon_id = icon_id;
                accounts_.save(account_key_, account_);
                x2::core::log_line(x2::core::LogLevel::Info, tag,
                                   std::format("account icon id={} body: {}", icon_id, proto::dump(frame.body)));
            }
            out = push_player_data(frame);
        } else if (opt == kFinishTask && !values.empty()) {
            // TitoGuide group finished: values [group, nextGroup] -> BaseInfo.QuestIDs.
            offline::finish_guide(account_, values[0], values.size() > 1 ? values[1] : 0);
            accounts_.save(account_key_, account_);
            out = push_player_data(frame);
        }
        proto::Writer body;
        body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
        body.int32(2, opt);
        const auto reply = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_Account),
                                                    frame.head.request_id, frame.head.session_id, data_version_,
                                                    static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
        out.insert(out.end(), reply.begin(), reply.end());
        return out;
    }

    if (tables_ && (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_CheckoutMainMissionSign) ||
                    frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_CheckoutMainMission))) {

        std::span<const std::uint8_t> checkout = frame.body;
        if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_CheckoutMainMissionSign)) {
            checkout = {};
            proto::Reader reader{frame.body};
            while (!reader.eof()) {
                const auto field = reader.field();
                if (!field) break;
                if (field->number == 1 && field->wire_type == 2) checkout = reader.bytes().value_or(checkout);
                else if (!reader.skip(field->wire_type)) break;
            }
        }
        const auto request = offline::parse_checkout(checkout);
        const auto result = offline::apply_checkout(account_, *tables_, request);
        accounts_.save(account_key_, account_);
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("checkout chapter={} section={} success={} first={} rewards={} level+{}",
                                       request.chapter, request.section, request.success, result.first_clear,
                                       result.rewards.size(), result.level_ups));
        auto out = push_player_data(frame);
        if (result.level_ups > 0) {
            const auto level = push_level_up(frame, account_.player.level - result.level_ups, account_.player.level);
            out.insert(out.end(), level.begin(), level.end());
        }
        if (!result.changed_items.empty()) {
            proto::Writer update; // L2C_ItemUpdate: 1 code, 2 repeated ItemData
            update.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
            for (const auto& item : result.changed_items) update.message(2, offline::ProtocolBuilders::item_data(item));
            const auto push = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_ItemUpdate), 0,
                                                       frame.head.session_id, ++data_version_,
                                                       static_cast<std::int32_t>(LogicCode::Ok), "", update.data());
            out.insert(out.end(), push.begin(), push.end());
        }
        if (!result.reward_equips.empty()) {
            proto::Writer update; // L2C_EquipUpdate: 1 code, 2 repeated HeroEquip
            update.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
            for (const auto& equip : result.reward_equips) {
                update.message(2, offline::ProtocolBuilders::hero_equip(equip));
            }
            const auto push = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_EquipUpdate), 0,
                                                       frame.head.session_id, ++data_version_,
                                                       static_cast<std::int32_t>(LogicCode::Ok), "", update.data());
            out.insert(out.end(), push.begin(), push.end());
        }
        if (!result.changed_heroes.empty()) {
            proto::Writer update; // L2C_HeroUpdate: 1 code, 2 repeated HeroData
            update.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
            for (const auto& hero : result.changed_heroes)
                update.message(2, offline::ProtocolBuilders::hero_data(hero, tables_.get()));
            const auto push = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_HeroUpdate), 0,
                                                       frame.head.session_id, ++data_version_,
                                                       static_cast<std::int32_t>(LogicCode::Ok), "", update.data());
            out.insert(out.end(), push.begin(), push.end());
        }
        const auto reply = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_CheckoutMainMission),
                                                    frame.head.request_id, frame.head.session_id, data_version_,
                                                    static_cast<std::int32_t>(LogicCode::Ok), "",
                                                    offline::checkout_reply(account_, request, result));
        out.insert(out.end(), reply.begin(), reply.end());
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_EquipAll)) {
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_EquipAll), frame.head.request_id,
                                         frame.head.session_id, data_version_,
                                         static_cast<std::int32_t>(LogicCode::Ok), "",
                                         offline::ProtocolBuilders::equip_all(account_));
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_QueryMission)) {
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_QueryMission), frame.head.request_id,
                                         frame.head.session_id, data_version_,
                                         static_cast<std::int32_t>(LogicCode::Ok), "",
                                         offline::ProtocolBuilders::query_mission(tables_.get(), account_));
    }

    if (tables_ && (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_RequestInsideBattleShop) ||
                    frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_BuyInsideBattleShopItems))) {
        // Request {1 shopID, 2 floor, 3 sectionId}; Buy {1 itemID, 2 itemNum = battle 棱镜 held, 3 sectionId}.
        std::array<std::int32_t, 4> f{};
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->number < f.size() && field->wire_type == 0)
                f[field->number] = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }
        if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_RequestInsideBattleShop))
            return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_RequestInsideBattleShop),
                                            frame.head.request_id, frame.head.session_id, data_version_,
                                            static_cast<std::int32_t>(LogicCode::Ok), "",
                                            offline::ProtocolBuilders::inside_battle_shop(*tables_, f[1], f[2], f[3]));
        // The client deducts itemPrice of priceType (棱镜 903) and grants the item itself.
        const auto price = offline::ProtocolBuilders::inside_battle_price(*tables_, f[1]);
        proto::Writer body; // 1 result (22 E_NO_ITEM, 35 E_LIMIT_GOLD), 2 itemID, 3 itemNum, 4 itemPrice, 5 priceType
        body.int32(1, price == 0 ? 22 : price > f[2] ? 35 : static_cast<std::int32_t>(LogicCode::Ok));
        body.int32(2, f[1]);
        body.int32(3, 1);
        body.int32(4, price);
        body.int32(5, 903);
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_BuyInsideBattleShopItems),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_FightDropData)) {
        // Sent when a battle layer starts. FightModule.OnFightDropData parses
        // f4 data as FightDropData and feeds it to LogicBattle as UpdateDropValue;
        // until that input arrives the layer never starts (hero cannot move).
        // Empty data = empty dropValues; bytes() still emits the field, so the
        // client gets a non-null byte[].
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("fight drop (mission f1, layer f3): {}", proto::dump(frame.body).substr(0, 160)));
        std::int32_t mission = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->number == 1 && field->wire_type == 0) mission = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }
        proto::Writer data; // FightDropData: 1 repeated dropValues (unpacked), 2 missionId
        if (tables_) {
            for (const auto value : offline::drop_values(*tables_, mission)) data.int32(1, value);
        }
        data.int32(2, mission);
        proto::Writer body; // L2C_FightDropData: 1 result, 2 uuid, 3 sign, 4 data
        body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
        body.string(2, now_token());
        body.bytes(4, data.data());
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_FightDropData),
                                         frame.head.request_id, frame.head.session_id, data_version_,
                                         static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_QueryReCommendShop)) {
        const auto body = offline::build_recommend_shop(account_, tables_.get());
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_QueryReCommendShop),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body);
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_ShopGoods)) {
        std::int32_t shop_id = 801;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->number == 1 && field->wire_type == 0)
                shop_id = static_cast<std::int32_t>(reader.varint().value_or(801));
            else if (!reader.skip(field->wire_type)) break;
        }
        const auto res = offline::query_shop_goods(account_, tables_.get(), shop_id, std::time(nullptr));
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_ShopGoods),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        res.code, "", res.body);
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_RefreshShop)) {
        std::int32_t shop_id = 801;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->number == 1 && field->wire_type == 0)
                shop_id = static_cast<std::int32_t>(reader.varint().value_or(801));
            else if (!reader.skip(field->wire_type)) break;
        }
        const auto res = offline::refresh_shop_goods(account_, tables_.get(), shop_id, std::time(nullptr));
        if (res.ok) accounts_.save(account_key_, account_);
        std::vector<std::uint8_t> out;
        if (res.wallet_changed) {
            const auto p = push_player_data(frame);
            out.insert(out.end(), p.begin(), p.end());
        }
        const auto& refresh_body = res.body.empty()
                                       ? [&] {
                                             proto::Writer fallback;
                                             fallback.int32(1, res.code);
                                             return fallback.data();
                                         }()
                                       : res.body;
        const auto reply = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_RefreshShop),
                                                    frame.head.request_id, frame.head.session_id, data_version_,
                                                    res.code, "", refresh_body);
        out.insert(out.end(), reply.begin(), reply.end());
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_BuyGoods)) {
        std::int32_t shop_id = 801;
        std::int32_t goods_id = 0;
        std::int32_t buy_num = 1;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->number == 1 && field->wire_type == 0)
                shop_id = static_cast<std::int32_t>(reader.varint().value_or(801));
            else if (field->number == 2 && field->wire_type == 0)
                goods_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->number == 3 && field->wire_type == 0)
                buy_num = static_cast<std::int32_t>(reader.varint().value_or(1));
            else if (!reader.skip(field->wire_type)) break;
        }
        const auto res = offline::buy_shop_goods(account_, tables_.get(), shop_id, goods_id, buy_num);
        if (res.ok) accounts_.save(account_key_, account_);
        std::vector<std::uint8_t> out;
        if (res.wallet_changed) {
            const auto p = push_player_data(frame);
            out.insert(out.end(), p.begin(), p.end());
        }
        if (!res.changed_items.empty()) {
            const auto item_push = push_item_updates(frame, res.changed_items);
            out.insert(out.end(), item_push.begin(), item_push.end());
        }
        // 失败时 body 必须带 {1: code}: 空体让客户端 code 解析为 0 且 rewardData=null,
        // OnBuyGoods 访问空 rewardData 直接 NRE 断线 (票据商店踢登录根因之一)。
        const auto& buy_body = res.body.empty()
                                   ? [&] {
                                         proto::Writer fallback;
                                         fallback.int32(1, res.code);
                                         return fallback.data();
                                     }()
                                   : res.body;
        const auto reply = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_BuyGoods),
                                                    frame.head.request_id, frame.head.session_id, data_version_,
                                                    res.code, "", buy_body);
        out.insert(out.end(), reply.begin(), reply.end());
        return out;
    }

    // ---- 终端: 时光(朋友圈) 484 → 485 + 通讯(角色私信) 476 → 477 ----
    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_QueryNpcBlog)) {
        const auto body = offline::query_npc_blog(account_, tables_.get(), std::time(nullptr));
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_QueryNpcBlog),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body);
    }
    // 时光点赞 488 → 489 / 回复 486 → 487: 回更新后的完整 blogBox (客户端 MergeBox 合并)
    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_LikeNpcBlog) ||
        frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_ReplyNpcBlog)) {
        std::int32_t hero_id = 0, group_id = 0, chat_group_id = 0, reply_id = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->wire_type == 0 && field->number <= 4) {
                const auto v = static_cast<std::int32_t>(reader.varint().value_or(0));
                if (field->number == 1) hero_id = v;
                else if (field->number == 2) group_id = v;
                else if (field->number == 3) chat_group_id = v;
                else if (field->number == 4) reply_id = v;
            } else if (!reader.skip(field->wire_type)) break;
        }
        std::vector<std::uint8_t> body;
        if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_LikeNpcBlog)) {
            body = offline::like_npc_blog(account_, tables_.get(), hero_id, group_id, std::time(nullptr));
        } else {
            body = offline::reply_npc_blog(account_, tables_.get(), hero_id, group_id, chat_group_id,
                                           reply_id, std::time(nullptr));
        }
        accounts_.save(account_key_, account_); // 赞/回复状态已持久化 (重登后 query 带状态)
        const auto reply_pid = frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_LikeNpcBlog)
                                   ? static_cast<std::uint32_t>(ProtoId::L2C_LikeNpcBlog)
                                   : static_cast<std::uint32_t>(ProtoId::L2C_ReplyNpcBlog);
        return marsnet::encode_response(reply_pid, frame.head.request_id, frame.head.session_id,
                                        data_version_, static_cast<std::int32_t>(LogicCode::Ok), "", body);
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_QueryPrivateLetter)) {
        const auto body = offline::query_npc_letter(account_, tables_.get(), std::time(nullptr));
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_QueryPrivateLetter),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body);
    }

    // 通讯读信上报 478 → 479: 回该英雄完整 letterBox (客户端 OnReceive 解引用 letterBox)
    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_UpdatePrivateLetter)) {
        std::int32_t hero_id = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->number == 1 && field->wire_type == 0)
                hero_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }
        const auto body = offline::update_private_letter(account_, tables_.get(), hero_id,
                                                         std::time(nullptr));
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_UpdatePrivateLetter),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body);
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_CommercialShopGoods)) {
        std::int32_t shop_type = 1;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->number == 1 && field->wire_type == 0)
                shop_type = static_cast<std::int32_t>(reader.varint().value_or(1));
            else if (!reader.skip(field->wire_type)) break;
        }
        if (shop_type <= 0) shop_type = 1;
        const auto res = offline::query_commercial_goods(account_, tables_.get(), shop_type);
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_CommercialShopGoods),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        res.code, "", res.body);
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_BuyCommercialGoods)) {
        std::int32_t goods_id = 0;
        std::int32_t shop_type = 1;
        std::int32_t currency_type = 902;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->number == 1 && field->wire_type == 0)
                goods_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->number == 2 && field->wire_type == 0)
                shop_type = static_cast<std::int32_t>(reader.varint().value_or(1));
            else if (field->number == 3 && field->wire_type == 0)
                currency_type = static_cast<std::int32_t>(reader.varint().value_or(902));
            else if (!reader.skip(field->wire_type)) break;
        }
        const auto res = offline::buy_commercial_goods(account_, tables_.get(), goods_id, shop_type, currency_type);
        if (res.ok) accounts_.save(account_key_, account_);
        std::vector<std::uint8_t> out;
        if (res.wallet_changed) {
            const auto p = push_player_data(frame);
            out.insert(out.end(), p.begin(), p.end());
        }
        if (!res.changed_items.empty()) {
            const auto item_push = push_item_updates(frame, res.changed_items);
            out.insert(out.end(), item_push.begin(), item_push.end());
        }
        if (res.ok) {
            // 购买皮肤成功：主动向客户端推送全量外观数据 L2C_HeroSkinAll (570)
            const auto skin_all_push = marsnet::encode_response(
                static_cast<std::uint32_t>(ProtoId::L2C_HeroSkinAll), 0,
                frame.head.session_id, ++data_version_,
                static_cast<std::int32_t>(LogicCode::Ok), "",
                offline::build_hero_skin_all(account_, tables_.get()));
            out.insert(out.end(), skin_all_push.begin(), skin_all_push.end());

            // 若能解析出该商品对应的神格，追加推送单神格增量 L2C_HeroSkinUpdate (571)
            std::int32_t target_hero_id = 0;
            if (tables_) {
                offline::GameTable item_tbl, app_tbl;
                if (item_tbl.load(*tables_, "Item") && app_tbl.load(*tables_, "Appearance")) {
                    if (const auto ir = item_tbl.row(static_cast<std::uint64_t>(res.item_id))) {
                        const auto eff = item_tbl.ints_field(*ir, 22);
                        if (!eff.empty()) {
                            if (const auto ar = app_tbl.row(static_cast<std::uint64_t>(eff[0]))) {
                                target_hero_id = app_tbl.int_field(*ar, 18).value_or(0);
                            }
                        }
                    }
                }
            }
            if (target_hero_id > 0) {
                const auto skin_update_push = marsnet::encode_response(
                    static_cast<std::uint32_t>(ProtoId::L2C_HeroSkinUpdate), 0,
                    frame.head.session_id, ++data_version_,
                    static_cast<std::int32_t>(LogicCode::Ok), "",
                    offline::build_hero_skin_update(target_hero_id, account_, tables_.get()));
                out.insert(out.end(), skin_update_push.begin(), skin_update_push.end());
            }
        }
        const auto reply = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_BuyCommercialGoods),
                                                    frame.head.request_id, frame.head.session_id, data_version_,
                                                    res.code, "", res.body);
        out.insert(out.end(), reply.begin(), reply.end());
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_HeroSkinAll)) {
        const auto body = offline::build_hero_skin_all(account_, tables_.get());
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_HeroSkinAll),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body);
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_HeroWearSkin)) {
        std::int32_t hero_id = 0;
        std::int32_t skin_id = 0;
        std::int32_t type = 0; // 1 = outerSkin (main), 2 = battleSkin, 3 = both
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->number == 1 && field->wire_type == 0)
                hero_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->number == 2 && field->wire_type == 0)
                skin_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->number == 3 && field->wire_type == 0)
                type = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }

        std::int32_t current_outer = 0;
        std::int32_t current_battle = 0;
        bool updated = false;
        for (auto& hero : account_.heroes) {
            if (hero.id == hero_id) {
                // 客户端协议规范 (逆向自 HeroMainSkinSubPage::OnMBtnApplyClick):
                // type 1: 战斗形象 (battleSkin)
                // type 2: 主界面形象 (outerSkin)
                // type 3: 同时设置为战斗形象 (outerSkin + battleSkin)
                if (type == 2 || type == 3) hero.outer_skin = skin_id;
                if (type == 1 || type == 3) hero.battle_skin = skin_id;
                current_outer = hero.outer_skin;
                current_battle = hero.battle_skin;
                updated = true;
                break;
            }
        }
        if (!updated) {
            account_.heroes.push_back(offline::AccountHero{
                .id = hero_id,
                .state = 2,
                .level = 1,
                .star = 1,
                .battle_skin = (type == 1 || type == 3) ? skin_id : 0,
                .outer_skin = (type == 2 || type == 3) ? skin_id : 0,
            });
            current_outer = account_.heroes.back().outer_skin;
            current_battle = account_.heroes.back().battle_skin;
            updated = true;
        }
        if (updated) accounts_.save(account_key_, account_);
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("wear skin hero={} skin={} type={} (cur_outer={} cur_battle={})",
                                       hero_id, skin_id, type, current_outer, current_battle));

        std::vector<std::uint8_t> out;
        const auto update_push = marsnet::encode_response(
            static_cast<std::uint32_t>(ProtoId::L2C_HeroSkinUpdate), 0,
            frame.head.session_id, ++data_version_,
            static_cast<std::int32_t>(LogicCode::Ok), "",
            offline::build_hero_skin_update(hero_id, account_, tables_.get()));
        out.insert(out.end(), update_push.begin(), update_push.end());

        proto::Writer reply_body;
        reply_body.int32(1, static_cast<std::int32_t>(LogicCode::Ok)); // 1: code = 10 (E_Ok)
        const auto reply = marsnet::encode_response(
            static_cast<std::uint32_t>(ProtoId::L2C_HeroWearSkin),
            frame.head.request_id, frame.head.session_id, data_version_,
            static_cast<std::int32_t>(LogicCode::Ok), "", reply_body.data());
        out.insert(out.end(), reply.begin(), reply.end());
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_ButtonClick)) {
        proto::Writer body;
        body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_ButtonClick),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_HeroAll)) {
        if (tables_ && offline::fix_worn_slots(account_, *tables_)) accounts_.save(account_key_, account_);
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_HeroAll),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "",
                                        offline::ProtocolBuilders::hero_all(account_, tables_.get()));
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_HeroOpt)) {
        const auto request = offline::parse_hero_opt(frame.body);
        const auto result = offline::apply_hero_opt(account_, tables_.get(), request);
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("hero opt id={} opt={} code={} body: {}", request.id, request.opt, result.code,
                                       proto::dump(frame.body)));
        if (result.code == static_cast<std::int32_t>(LogicCode::Ok)) accounts_.save(account_key_, account_);
        std::vector<std::uint8_t> out;
        const auto append = [&out](const std::vector<std::uint8_t>& bytes) {
            out.insert(out.end(), bytes.begin(), bytes.end());
        };
        if (result.player) append(push_player_data(frame));
        const auto push_update = [&](ProtoId pid, const std::vector<std::vector<std::uint8_t>>& entries) {
            proto::Writer body;
            body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
            for (const auto& entry : entries) body.message(2, entry);
            append(marsnet::encode_response(static_cast<std::uint32_t>(pid), 0, frame.head.session_id,
                                            ++data_version_, static_cast<std::int32_t>(LogicCode::Ok), "",
                                            body.data()));
        };
        if (!result.heroes.empty()) {
            std::vector<std::vector<std::uint8_t>> entries;
            for (const auto& hero : result.heroes)
                entries.push_back(offline::ProtocolBuilders::hero_data(hero, tables_.get()));
            push_update(ProtoId::L2C_HeroUpdate, entries);
        }
            // 升星/解锁可能跨 5 星阈值 (E_Stage 觉醒皮肤+星级头像) → 推单英雄外观全量
            if (request.opt == 0 || request.opt == 2) {
                for (const auto& hero : result.heroes) {
                    append(marsnet::encode_response(
                        static_cast<std::uint32_t>(ProtoId::L2C_HeroSkinUpdate), 0,
                        frame.head.session_id, ++data_version_,
                        static_cast<std::int32_t>(LogicCode::Ok), "",
                        offline::build_hero_skin_update(hero.id, account_, tables_.get())));
                }
            }
        if (!result.items.empty()) {
            std::vector<std::vector<std::uint8_t>> entries;
            proto::Writer removed;
            for (const auto& item : result.items) {
                if (item.num > 0) entries.push_back(offline::ProtocolBuilders::item_data(item));
                else removed.int32(1, item.id); // L2C_ItemRemove.ids; num 0 stays visible if only updated
            }
            if (!entries.empty()) push_update(ProtoId::L2C_ItemUpdate, entries);
            if (!removed.data().empty()) {
                append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_ItemRemove), 0,
                                                frame.head.session_id, ++data_version_,
                                                static_cast<std::int32_t>(LogicCode::Ok), "", removed.data()));
            }
        }
        proto::Writer body; // L2C_HeroOpt: 1 code, 2 id, 3 opt, 4 rewardData
        body.int32(1, result.code);
        body.int64(2, request.id);
        body.int32(3, request.opt);
        body.message(4, {});
        if (request.consume_item > 0) body.int32(5, request.consume_item);
        append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_HeroOpt), frame.head.request_id,
                                        frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data()));
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_HeroGodLike)) {
        std::int32_t hero_id = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->number == 1 && field->wire_type == 0) hero_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }
        const auto result = offline::apply_god_like(account_, tables_.get(), hero_id);
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("god like hero={} code={} body: {}", hero_id, result.code, proto::dump(frame.body)));
        if (result.code == static_cast<std::int32_t>(LogicCode::Ok)) accounts_.save(account_key_, account_);
        std::vector<std::uint8_t> out;
        const auto append = [&out](const std::vector<std::uint8_t>& bytes) {
            out.insert(out.end(), bytes.begin(), bytes.end());
        };
        const auto push_update = [&](ProtoId pid, const std::vector<std::vector<std::uint8_t>>& entries) {
            proto::Writer body;
            body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
            for (const auto& entry : entries) body.message(2, entry);
            append(marsnet::encode_response(static_cast<std::uint32_t>(pid), 0, frame.head.session_id,
                                            ++data_version_, static_cast<std::int32_t>(LogicCode::Ok), "",
                                            body.data()));
        };
        if (!result.heroes.empty()) {
            std::vector<std::vector<std::uint8_t>> entries;
            for (const auto& hero : result.heroes)
                entries.push_back(offline::ProtocolBuilders::hero_data(hero, tables_.get()));
            push_update(ProtoId::L2C_HeroUpdate, entries);
        }
        if (!result.items.empty()) {
            std::vector<std::vector<std::uint8_t>> entries;
            proto::Writer removed;
            for (const auto& item : result.items) {
                if (item.num > 0) entries.push_back(offline::ProtocolBuilders::item_data(item));
                else removed.int32(1, item.id);
            }
            if (!entries.empty()) push_update(ProtoId::L2C_ItemUpdate, entries);
            if (!removed.data().empty()) {
                append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_ItemRemove), 0,
                                                frame.head.session_id, ++data_version_,
                                                static_cast<std::int32_t>(LogicCode::Ok), "", removed.data()));
            }
        }
        proto::Writer body; // L2C_HeroGodLike: 1 code. Client refreshes only when this is E_Ok.
        body.int32(1, result.code);
        append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_HeroGodLike), frame.head.request_id,
                                        frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data()));
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_GodSlotLock)) {
        std::int32_t hero_id = 0, slot = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->wire_type == 0 && field->number == 1) hero_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->wire_type == 0 && field->number == 2) slot = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }
        const auto result = offline::apply_god_slot(account_, tables_.get(), hero_id, slot);
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("god slot hero={} slot={} code={} body: {}", hero_id, slot, result.code,
                                       proto::dump(frame.body)));
        if (result.code == static_cast<std::int32_t>(LogicCode::Ok)) accounts_.save(account_key_, account_);
        std::vector<std::uint8_t> out;
        const auto append = [&out](const std::vector<std::uint8_t>& bytes) {
            out.insert(out.end(), bytes.begin(), bytes.end());
        };
        const auto push_update = [&](ProtoId pid, const std::vector<std::vector<std::uint8_t>>& entries) {
            proto::Writer body;
            body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
            for (const auto& entry : entries) body.message(2, entry);
            append(marsnet::encode_response(static_cast<std::uint32_t>(pid), 0, frame.head.session_id,
                                            ++data_version_, static_cast<std::int32_t>(LogicCode::Ok), "",
                                            body.data()));
        };
        if (!result.heroes.empty()) {
            std::vector<std::vector<std::uint8_t>> entries;
            for (const auto& hero : result.heroes)
                entries.push_back(offline::ProtocolBuilders::hero_data(hero, tables_.get()));
            push_update(ProtoId::L2C_HeroUpdate, entries);
        }
        if (!result.items.empty()) {
            std::vector<std::vector<std::uint8_t>> entries;
            proto::Writer removed;
            for (const auto& item : result.items) {
                if (item.num > 0) entries.push_back(offline::ProtocolBuilders::item_data(item));
                else removed.int32(1, item.id);
            }
            if (!entries.empty()) push_update(ProtoId::L2C_ItemUpdate, entries);
            if (!removed.data().empty()) {
                append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_ItemRemove), 0,
                                                frame.head.session_id, ++data_version_,
                                                static_cast<std::int32_t>(LogicCode::Ok), "", removed.data()));
            }
        }
        proto::Writer body; // L2C_GodSlotLock: 1 code, 2 heroID, 3 slotIndex
        body.int32(1, result.code);
        body.int32(2, hero_id);
        body.int32(3, slot);
        append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_GodSlotLock), frame.head.request_id,
                                        frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data()));
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_Artifact)) {
        std::int32_t opt = 0, hero_id = 0, jewel_id = 0, hole_id = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->wire_type == 0 && field->number == 1) opt = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->wire_type == 0 && field->number == 2) hero_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->wire_type == 0 && field->number == 3) jewel_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->wire_type == 0 && field->number == 4) hole_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }
        const auto result = offline::handle_artifact_opt(account_, tables_.get(), opt, hero_id, jewel_id, hole_id);
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("artifact opt={} hero={} jewel={} hole={} ok={} code={} body: {}",
                                       opt, hero_id, jewel_id, hole_id, result.ok, result.code, proto::dump(frame.body)));
        if (result.ok) accounts_.save(account_key_, account_);
        std::vector<std::uint8_t> out;
        const auto append = [&out](const std::vector<std::uint8_t>& bytes) {
            out.insert(out.end(), bytes.begin(), bytes.end());
        };
        if (result.player_currency_changed) append(push_player_data(frame));
        const auto push_update = [&](ProtoId pid, const std::vector<std::vector<std::uint8_t>>& entries) {
            proto::Writer body;
            body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
            for (const auto& entry : entries) body.message(2, entry);
            append(marsnet::encode_response(static_cast<std::uint32_t>(pid), 0, frame.head.session_id,
                                            ++data_version_, static_cast<std::int32_t>(LogicCode::Ok), "",
                                            body.data()));
        };
        if (!result.heroes.empty()) {
            std::vector<std::vector<std::uint8_t>> entries;
            for (const auto& hero : result.heroes)
                entries.push_back(offline::ProtocolBuilders::hero_data(hero, tables_.get()));
            push_update(ProtoId::L2C_HeroUpdate, entries);
        }
        if (!result.items.empty()) {
            std::vector<std::vector<std::uint8_t>> entries;
            proto::Writer removed;
            for (const auto& item : result.items) {
                if (item.num > 0) entries.push_back(offline::ProtocolBuilders::item_data(item));
                else removed.int32(1, item.id);
            }
            if (!entries.empty()) push_update(ProtoId::L2C_ItemUpdate, entries);
            if (!removed.data().empty()) {
                append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_ItemRemove), 0,
                                                frame.head.session_id, ++data_version_,
                                                static_cast<std::int32_t>(LogicCode::Ok), "", removed.data()));
            }
        }
        proto::Writer body; // L2C_Artifact: 1 result, 2 opt, 3 HeroID, 4 JewelID, 5 HoleIsID
        body.int32(1, static_cast<std::int32_t>(result.ok ? LogicCode::Ok : LogicCode::ErrorOpt));
        body.int32(2, opt);
        body.int32(3, hero_id);
        body.int32(4, jewel_id);
        body.int32(5, hole_id);
        append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_Artifact), frame.head.request_id,
                                        frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data()));
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_UpHeroSkill)) {
        std::int32_t hero_id = 0, skill_id = 0, uplevel = 1;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->wire_type == 0 && field->number == 1) hero_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->wire_type == 0 && field->number == 2) skill_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->wire_type == 0 && field->number == 3) uplevel = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }
        const auto result = offline::apply_hero_skill_up(account_, tables_.get(), hero_id, skill_id, uplevel);
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("skill up hero={} skill={} uplevel={} code={} body: {}",
                                       hero_id, skill_id, uplevel, result.code, proto::dump(frame.body)));
        if (result.code == static_cast<std::int32_t>(LogicCode::Ok)) {
            accounts_.save(account_key_, account_);
        }
        std::vector<std::uint8_t> out;
        const auto append = [&out](const std::vector<std::uint8_t>& bytes) {
            out.insert(out.end(), bytes.begin(), bytes.end());
        };
        if (result.player) append(push_player_data(frame));
        const auto push_update = [&](ProtoId pid, const std::vector<std::vector<std::uint8_t>>& entries) {
            proto::Writer body;
            body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
            for (const auto& entry : entries) body.message(2, entry);
            append(marsnet::encode_response(static_cast<std::uint32_t>(pid), 0, frame.head.session_id,
                                            ++data_version_, static_cast<std::int32_t>(LogicCode::Ok), "",
                                            body.data()));
        };
        if (!result.heroes.empty()) {
            std::vector<std::vector<std::uint8_t>> entries;
            for (const auto& hero : result.heroes)
                entries.push_back(offline::ProtocolBuilders::hero_data(hero, tables_.get()));
            push_update(ProtoId::L2C_HeroUpdate, entries);
        }
        if (!result.items.empty()) {
            std::vector<std::vector<std::uint8_t>> entries;
            proto::Writer removed;
            for (const auto& item : result.items) {
                if (item.num > 0) entries.push_back(offline::ProtocolBuilders::item_data(item));
                else removed.int32(1, item.id);
            }
            if (!entries.empty()) push_update(ProtoId::L2C_ItemUpdate, entries);
            if (!removed.data().empty()) {
                append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_ItemRemove), 0,
                                                frame.head.session_id, ++data_version_,
                                                static_cast<std::int32_t>(LogicCode::Ok), "", removed.data()));
            }
        }
        proto::Writer body; // L2C_UpHeroSkill: 1 code, 2 heroId, 3 skillId, 4 uplevel
        body.int32(1, result.code);
        body.int32(2, hero_id);
        body.int32(3, skill_id);
        body.int32(4, uplevel);
        append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_UpHeroSkill), frame.head.request_id,
                                        frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data()));
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_AddFavor)) {
        offline::AddFavorRequest req;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->wire_type == 0 && field->number == 1) req.opt = static_cast<std::int32_t>(reader.varint().value_or(2));
            else if (field->wire_type == 0 && field->number == 2) req.option_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->wire_type == 0 && field->number == 3) req.hero_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->wire_type == 0 && field->number == 4) req.num = static_cast<std::int32_t>(reader.varint().value_or(1));
            else if (!reader.skip(field->wire_type)) break;
        }

        const auto res = offline::apply_add_favor(account_, tables_.get(), req);
        if (res.ok) {
            accounts_.save(account_key_, account_);
        }

        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("add favor hero={} opt={} item={} num={} ok={} code={} body: {}",
                                       req.hero_id, req.opt, req.option_id, req.num, res.ok, res.code, proto::dump(frame.body)));

        std::vector<std::uint8_t> out;
        const auto append = [&out](const std::vector<std::uint8_t>& bytes) {
            out.insert(out.end(), bytes.begin(), bytes.end());
        };

        if (res.ok) {
            // 1. 推送 L2C_ItemUpdate (553) (仅送礼消耗道具时推送)
            if (res.opt == 2 && res.item_id > 0) {
                proto::Writer item_update;
                item_update.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
                offline::AccountItem changed_item{.id = res.item_id, .num = res.new_item_count};
                item_update.message(2, offline::ProtocolBuilders::item_data(changed_item));
                append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_ItemUpdate), 0,
                                                frame.head.session_id, ++data_version_,
                                                static_cast<std::int32_t>(LogicCode::Ok), "",
                                                item_update.data()));
            }

            // 2. 推送 L2C_HeroUpdate (549)
            proto::Writer hero_update;
            hero_update.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
            hero_update.message(2, offline::ProtocolBuilders::hero_data(res.updated_hero, tables_.get()));
            append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_HeroUpdate), 0,
                                            frame.head.session_id, ++data_version_,
                                            static_cast<std::int32_t>(LogicCode::Ok), "",
                                            hero_update.data()));

            // 3. 推送 PlayerDataProto (1000) 确保客户端 FavorMap 全量同步
            append(push_player_data(frame));

            // 4. 推送 L2C_FavorChangeInfo (497) 触发客户端好感度增长动画
            proto::Writer info;
            info.int32(1, res.before_level);
            info.int32(2, res.before_exp);
            info.int32(3, res.after_level);
            info.int32(4, res.after_exp);
            info.int32(5, res.hero_id);
            info.int32(6, res.opt == 1 ? 2 : 7); // 2 = Interactive, 7 = Gifts
            proto::Writer favor_change;
            favor_change.message(1, info.data());
            append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_FavorChangeInfo), 0,
                                            frame.head.session_id, ++data_version_,
                                            static_cast<std::int32_t>(LogicCode::Ok), "",
                                            favor_change.data()));

            // 5. 若有语音解锁，推送 L2C_SaveHeroDubbing (689)
            if (!res.unlocked_dubbings.empty()) {
                proto::Writer dubbing_body;
                dubbing_body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
                proto::Writer dubbing_entry;
                dubbing_entry.int32(1, res.hero_id);
                for (const auto dub_id : res.unlocked_dubbings) dubbing_entry.int32(2, dub_id);
                dubbing_body.message(2, dubbing_entry.data());
                append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_SaveHeroDubbing), 0,
                                                frame.head.session_id, ++data_version_,
                                                static_cast<std::int32_t>(LogicCode::Ok), "",
                                                dubbing_body.data()));
            }
        }

        // 5. 应答 L2C_AddFavor (277)
        // L2C_AddFavor 字段: 1 code, 2 opt, 3 optionId, 4 heroId, 5 exp, 6 level, 7 newExp, 8 newLevel, 9 giftsTimes
        proto::Writer body;
        body.int32(1, res.code);
        body.int32(2, res.opt);
        body.int32(3, res.option_id);
        body.int32(4, res.hero_id);
        body.int32(5, res.before_exp);
        body.int32(6, res.before_level);
        body.int32(7, res.after_exp);
        body.int32(8, res.after_level);
        body.int32(9, res.gifts_times);
        append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_AddFavor), frame.head.request_id,
                                        frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data()));
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_GodEquipJewelDot)) {
        proto::Writer body; // L2C_GodEquipJewelDot: 1 code
        body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_GodEquipJewelDot),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_UnlockHeroArchives)) {
        std::int32_t hero_id = 0, archives_id = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->wire_type == 0 && field->number == 1) hero_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->wire_type == 0 && field->number == 2) archives_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }

        auto it = std::find_if(account_.heroes.begin(), account_.heroes.end(),
                               [hero_id](const auto& h) { return h.id == hero_id; });
        const bool found = (it != account_.heroes.end() && archives_id > 0);
        if (found) {
            it->archives[archives_id] = 2; // 2: Unlocked
            accounts_.save(account_key_, account_);
        }

        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("unlock archives hero={} archive={} ok={} body: {}",
                                       hero_id, archives_id, found, proto::dump(frame.body)));

        std::vector<std::uint8_t> out;
        const auto append = [&out](const std::vector<std::uint8_t>& bytes) {
            out.insert(out.end(), bytes.begin(), bytes.end());
        };

        if (found) {
            proto::Writer update_body;
            update_body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
            update_body.message(2, offline::ProtocolBuilders::hero_data(*it, tables_.get()));
            append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_HeroUpdate), 0,
                                            frame.head.session_id, ++data_version_,
                                            static_cast<std::int32_t>(LogicCode::Ok), "",
                                            update_body.data()));
        }

        proto::Writer body; // L2C_UnlockHeroArchives: 1 code, 2 heroID, 3 archivesID
        body.int32(1, static_cast<std::int32_t>(found ? LogicCode::Ok : LogicCode::ErrorOpt));
        body.int32(2, hero_id);
        body.int32(3, archives_id);
        append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_UnlockHeroArchives),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data()));
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_QueryHeroJournal)) {
        std::int32_t hero_id = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->wire_type == 0 && field->number == 1) {
                hero_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            } else if (!reader.skip(field->wire_type)) {
                break;
            }
        }

        int cur_favor = 1;
        const auto it = std::find_if(account_.heroes.begin(), account_.heroes.end(),
                                     [hero_id](const auto& h) { return h.id == hero_id; });
        if (it != account_.heroes.end() && it->favor_level > 0) {
            cur_favor = it->favor_level;
        }

        std::vector<std::int32_t> journals;
        if (tables_) {
            offline::GameTable table;
            if (table.load(*tables_, "FavorabilityDairy")) {
                for (const auto& row : table.rows()) {
                    const auto hid = table.int_field(row, 2).value_or(0);
                    if (hid != hero_id) continue;
                    const auto did = table.int_field(row, 1).value_or(0);
                    if (did <= 0) continue;
                    const auto trigger_type = table.int_field(row, 3).value_or(0);
                    const auto type_num = table.int_field(row, 4).value_or(0);
                    if (trigger_type == 3 || trigger_type == 0 || trigger_type == 2 || (trigger_type == 1 && cur_favor >= type_num)) {
                        journals.push_back(did);
                    }
                }
            }
        }

        if (journals.empty()) {
            if (tables_) {
                offline::GameTable table;
                if (table.load(*tables_, "FavorabilityDairy")) {
                    for (const auto& row : table.rows()) {
                        const auto hid = table.int_field(row, 2).value_or(0);
                        if (hid == hero_id) {
                            const auto did = table.int_field(row, 1).value_or(0);
                            if (did > 0) journals.push_back(did);
                        }
                    }
                }
            }
        }
        if (journals.empty()) {
            journals.push_back(6000000 + hero_id * 10 + 1);
        }

        std::ranges::sort(journals);
        const auto [last, end] = std::ranges::unique(journals);
        journals.erase(last, end);

        proto::Writer body; // L2C_QueryHeroJournal: 1 code, 2 heroId, 3 repeated int32 journal
        body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
        body.int32(2, hero_id);
        for (const auto did : journals) {
            body.int32(3, did);
        }

        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("query hero journal hero={} count={} body: {}",
                                       hero_id, journals.size(), proto::dump(frame.body)));

        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_QueryHeroJournal),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_BuildingUpgrade)) {
        // 白夜行星 701-708 建筑升级 (学院基地/白夜大厅等). The reply's field-2
        // buildingId is what the client's OnReceiveBuildUpgrade forwards as event
        // 206 (refresh); an empty body refreshes building 0 and the click looks
        // dead. The build itself now runs for CollegeLevel.f8 seconds: pay the
        // row's f9/f10 costs (1237901 金币 from the wallet, materials from the
        // bag), queue it in growthBase.buildQueue, and complete it when the
        // timer expires (heartbeat-polled, see poll_pushes).
        std::int32_t building_id = 0, build_type = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->wire_type == 0 && field->number == 1) building_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->wire_type == 0 && field->number == 2) build_type = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }

        const auto now = static_cast<std::int32_t>(std::time(nullptr));
        std::vector<std::uint8_t> out;
        const auto append = [&out](const std::vector<std::uint8_t>& bytes) {
            out.insert(out.end(), bytes.begin(), bytes.end());
        };
        // Lazy-finish an expired build first so the queue slot frees up.
        if (const auto done = offline::finish_expired_college_build(account_, now)) {
            accounts_.save(account_key_, account_);
            append(college_done_pushes(frame, done));
        }
        const auto res = offline::start_college_upgrade(account_, tables_.get(), building_id, now);
        if (res.ok) accounts_.save(account_key_, account_);

        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("college building upgrade id={} type={} lv={} end=+{}s ok={} code={} body: {}",
                                       building_id, build_type, res.level, res.duration_sec, res.ok, res.code,
                                       proto::dump(frame.body)));

        proto::Writer body; // L2C_BuildingUpgrade: 1 code, 2 buildingId, 3 type, 4 endTime, 5 buildTime
        body.int32(1, res.ok ? static_cast<std::int32_t>(LogicCode::Ok) : res.code);
        body.int32(2, building_id);
        body.int32(3, res.type);
        body.int32(4, offline::client_time(res.end_unix)); // countdown target (client epoch)
        body.int32(5, res.duration_sec);
        append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_BuildingUpgrade),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data()));
        if (res.ok) { // countdown state + spent costs
            append(push_item_updates(frame, res.changed_items));
            if (res.wallet_changed) append(push_player_data(frame));
            append(push_growth(frame));
        }
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_BuildSpeedUP)) {
        // 升级加速/钻石立即完成: pay itemId×itemNum and finish the build now.
        std::int32_t building_id = 0, build_type = 0, item_id = 0, item_num = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->wire_type == 0 && field->number == 1) building_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->wire_type == 0 && field->number == 2) build_type = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->wire_type == 0 && field->number == 3) item_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->wire_type == 0 && field->number == 4) item_num = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }

        const auto now = static_cast<std::int32_t>(std::time(nullptr));
        std::vector<std::uint8_t> out;
        const auto append = [&out](const std::vector<std::uint8_t>& bytes) {
            out.insert(out.end(), bytes.begin(), bytes.end());
        };
        std::int32_t code = offline::kCollegeOk;
        bool finished = false, wallet_changed = false;
        std::vector<offline::AccountItem> changed;
        if (const auto done = offline::finish_expired_college_build(account_, now)) {
            // Timer already ran out: complete for free.
            accounts_.save(account_key_, account_);
            building_id = done;
            finished = true;
        } else {
            const auto res = offline::speed_up_college_build(account_, tables_.get(), building_id,
                                                             item_id, item_num, now);
            code = res.code;
            finished = res.ok;
            wallet_changed = res.wallet_changed;
            changed = std::move(res.changed_items);
            if (res.ok) accounts_.save(account_key_, account_);
        }

        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("college speed-up id={} item={}x{} ok={} code={} body: {}",
                                       building_id, item_id, item_num, finished, code,
                                       proto::dump(frame.body)));

        proto::Writer body; // L2C_BuildSpeedUP: 1 code, 2 buildingId, 3 type, 4 endTime,
                            // 5 buildTime, 6 itemId, 7 itemNum, 8 buildStatus
        body.int32(1, finished ? static_cast<std::int32_t>(LogicCode::Ok) : code);
        body.int32(2, building_id);
        body.int32(3, build_type);
        body.int32(4, offline::client_time(now));
        body.int32(5, 0);
        body.int32(6, item_id);
        body.int32(7, item_num);
        body.int32(8, 1); // buildStatus: finished
        append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_BuildSpeedUP),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data()));
        if (finished) {
            append(push_item_updates(frame, changed));
            if (wallet_changed) append(push_player_data(frame));
            if (building_id > 0) append(college_done_pushes(frame, building_id));
        }
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_BuildStarUP)) {
        // 建造/升星 (CollegeUpgradeModule.WonderBuildUp + building star-up). The
        // tutorial (TitoGuide 21184) auto-clicks WonderBuild/mBtn_LevelUp on an
        // unbuilt wonder; the star-1 rows are free so that first 建造 costs
        // nothing. Higher stars pay the CollegeStarLevel row costs (f9/f10).
        // Reply 246, then push a fresh 584 growthBase: OnGetQueryGrowthBaseData
        // replaces growthData and fires event 257, which re-runs
        // RefreshAllCivalization for the built wonder.
        std::int32_t building_id = 0, build_type = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->wire_type == 0 && field->number == 1) building_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->wire_type == 0 && field->number == 2) build_type = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }

        const auto res = offline::star_up_college(account_, tables_.get(), building_id);
        if (res.ok) accounts_.save(account_key_, account_);

        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("college build star-up id={} type={} star={} ok={} code={} body: {}",
                                       building_id, res.type, res.star, res.ok, res.code,
                                       proto::dump(frame.body)));

        std::vector<std::uint8_t> out;
        const auto append = [&out](const std::vector<std::uint8_t>& bytes) {
            out.insert(out.end(), bytes.begin(), bytes.end());
        };
        append(push_item_updates(frame, res.changed_items));
        if (res.ok && res.wallet_changed) append(push_player_data(frame));
        proto::Writer body; // L2C_BuildStarUP: 1 code, 2 buildingId, 3 type, 4 star
        body.int32(1, res.ok ? static_cast<std::int32_t>(LogicCode::Ok) : res.code);
        body.int32(2, building_id);
        body.int32(3, res.type);
        body.int32(4, res.star);
        append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_BuildStarUP),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data()));
        if (res.ok) { // refreshed growth base -> event 257 refreshes the wonder UI
            append(push_growth(frame));
        }
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_QueryGrowthBase)) {
        // CollegeEntry.OnOpen asks for the same 白夜行星 data L2C_Login.f14
        // already delivered; answer with the identical body. A build whose
        // timer expired while nobody asked completes here first.
        const auto now = static_cast<std::int32_t>(std::time(nullptr));
        if (const auto done = offline::finish_expired_college_build(account_, now)) {
            accounts_.save(account_key_, account_);
            std::vector<std::uint8_t> out = college_done_pushes(frame, done);
            const auto reply = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_QueryGrowthBase),
                                                        frame.head.request_id, frame.head.session_id,
                                                        data_version_, static_cast<std::int32_t>(LogicCode::Ok),
                                                        "", offline::ProtocolBuilders::growth_base(account_, tables_.get()));
            out.insert(out.end(), reply.begin(), reply.end());
            return out;
        }
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_QueryGrowthBase),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "",
                                        offline::ProtocolBuilders::growth_base(account_, tables_.get()));
    }

    // ---- 成就系统 (AchievementModule) ----
    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_AchvOverView)) {
        // 345 → 348: 4 个页签 (Group 1成长 2玩法 3社交 4默契度) 的已完成数 + 点数。
        // AchvOverViewData: 1 achvType, 2 achvProgress (该组已完成成就数)。
        if (tables_) offline::roll_daily_tasks(account_, *tables_, std::time(nullptr));
        proto::Writer body; // L2C_AchvOverView: 1 code, 2 repeated overview, 3 point, 4 max, 5 claimed point rewards
        body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
        offline::GameTable achievement;
        if (achievement.load(*tables_, "Achievement")) {
            std::int32_t completed[5] = {};
            std::int32_t total[5] = {};
            for (const auto& row : achievement.rows()) {
                if (achievement.int_field(row, 10).value_or(1) != 1) continue; // IsUse
                const auto group = achievement.int_field(row, 8).value_or(0);
                if (group < 1 || group > 4) continue;
                ++total[group];
                const auto id = static_cast<std::int32_t>(row.key);
                // 组完成数 = 已领取数 (与点数同口径: 达标未领不算完成)
                if (std::ranges::contains(account_.player.achv_claimed, id)) ++completed[group];
            }
            for (std::int32_t g = 1; g <= 4; ++g) {
                proto::Writer entry;
                entry.int32(1, g);
                entry.int32(2, completed[g]);
                body.message(2, entry.data());
                (void)total[g];
            }
        }
        const auto points = offline::achv_total_points(account_, tables_.get());
        body.int32(3, points);
        // f5 = 每个里程碑的 AchvStatus 数组: 0=Ing 未达标, 1=Reward 可领取, 2=Finish 已领取。
        // RefreshOverView 从首个取值 ∈{0,1} 的下标定位 curGiftIndex (首个未领取项),
        // 再用 points >= CompleteAchievementNumber[curGiftIndex] 决定领取按钮亮灭;
        // 发布尔 0/1 会把 curGiftIndex 钉死在下标 0 (已领项也命中 1)。
        const auto thresholds = offline::achv_point_thresholds(tables_.get());
        body.int32(4, thresholds.empty() ? 0 : thresholds.back());
        for (std::int32_t idx = 0; idx < static_cast<std::int32_t>(thresholds.size()); ++idx) {
            if (std::ranges::contains(account_.player.achv_point_claimed, idx)) body.int32(5, 2);
            else body.int32(5, points >= thresholds[idx] ? 1 : 0);
        }
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_AchvOverView),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
    }

    if (frame.proto_id == 344) { // C2L_AchvDetialData (枚举名缺失, 数值 344)
        // {1 achvType, 2 startIndex, 3 endIndex} → 347 {1 code, 2 repeated AchvData, 3 achvType}
        std::int32_t achv_type = 0, start_index = 0, end_index = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->wire_type == 0 && field->number == 1) achv_type = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->wire_type == 0 && field->number == 2) start_index = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->wire_type == 0 && field->number == 3) end_index = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }
        proto::Writer body; // L2C_AchvDetialData
        body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
        std::int32_t count = 0;
        if (tables_) {
            offline::GameTable achievement;
            if (achievement.load(*tables_, "Achievement")) {
                // 按页签过滤 + id 升序 + LastAchievement 链序 (前置在前)
                std::vector<std::pair<std::int32_t, std::int32_t>> list; // {id, prev}
                for (const auto& row : achievement.rows()) {
                    if (achievement.int_field(row, 10).value_or(1) != 1) continue;
                    if (achievement.int_field(row, 8).value_or(0) != achv_type) continue;
                    list.emplace_back(static_cast<std::int32_t>(row.key), achievement.int_field(row, 7).value_or(0));
                }
                std::ranges::sort(list);
                // 简单拓扑: 前置在前 (id 升序已满足 66xxxx 链编号规则)
                for (std::size_t idx = 0; idx < list.size(); ++idx) {
                    if (idx < static_cast<std::size_t>(start_index)) continue;
                    if (end_index > 0 && idx >= static_cast<std::size_t>(end_index)) break;
                    const auto id = list[idx].first;
                    const auto progress = offline::achv_progress(id, account_, tables_.get());
                    const auto target = offline::achv_target(id, tables_.get());
                    const auto done = progress >= target;
                    const auto claimed = std::ranges::contains(account_.player.achv_claimed, id);
                    proto::Writer entry; // AchvData: 1 progress, 2 status, 3 achvId, 4 stage
                    entry.int32(1, progress);
                    entry.int32(2, claimed ? 2 : (done ? 1 : 0)); // Finish / Reward / Ing
                    entry.int32(3, id);
                    entry.int32(4, offline::achv_stage(id, account_, tables_.get()));
                    body.message(2, entry.data());
                    ++count;
                }
            }
        }
        body.int32(3, achv_type);
        (void)count;
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_AchvDetialData),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_AchvReward)) {
        // 346 → 349: 领成就奖励。{1 achvId}
        std::int32_t achv_id = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->wire_type == 0 && field->number == 1) achv_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }
        const auto res = offline::claim_achievement(account_, tables_.get(), achv_id);
        if (res.ok) accounts_.save(account_key_, account_);
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("achv reward id={} ok={} code={} body: {}",
                                       achv_id, res.ok, res.code, proto::dump(frame.body)));
        std::vector<std::uint8_t> out;
        const auto append = [&out](const std::vector<std::uint8_t>& bytes) {
            out.insert(out.end(), bytes.begin(), bytes.end());
        };
        append(push_item_updates(frame, res.bag));
        if (res.ok && res.player) append(push_player_data(frame));
        proto::Writer body; // L2C_AchvReward: 1 code, 2 status, 3 achvId, 4 RewardData
        body.int32(1, res.ok ? static_cast<std::int32_t>(LogicCode::Ok) : res.code);
        body.int32(2, res.ok ? res.status : 0);
        body.int32(3, achv_id);
        if (res.ok) {
            proto::Writer reward; // RewardData: 1 repeated RewardItem{1 itemId, 2 itemNum}
            for (const auto& [item_id, num] : res.shown) {
                proto::Writer entry;
                entry.int32(1, item_id);
                entry.int32(2, num);
                reward.message(1, entry.data());
            }
            body.message(4, reward.data());
        }
        append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_AchvReward),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data()));
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_AchvPointReward)) {
        // 358 → 359: 领成就点数里程碑宝箱。{1 achvPointId}
        std::int32_t point_id = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->wire_type == 0 && field->number == 1) point_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }
        const auto res = offline::claim_achievement_point(account_, tables_.get(), point_id);
        if (res.ok) accounts_.save(account_key_, account_);
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("achv point reward id={} ok={} code={} body: {}",
                                       point_id, res.ok, res.code, proto::dump(frame.body)));
        std::vector<std::uint8_t> out;
        if (res.ok) {
            if (res.player) out = push_player_data(frame);
            if (!res.bag.empty()) {
                proto::Writer update;
                update.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
                for (const auto& item : res.bag) update.message(2, offline::ProtocolBuilders::item_data(item));
                const auto pushed = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_ItemUpdate), 0,
                                                             frame.head.session_id, ++data_version_,
                                                             static_cast<std::int32_t>(LogicCode::Ok), "", update.data());
                out.insert(out.end(), pushed.begin(), pushed.end());
            }
        }
        proto::Writer body; // L2C_AchvPointReward: 1 code, 2 status, 3 achvPointId, 4 RewardData
        body.int32(1, res.ok ? static_cast<std::int32_t>(LogicCode::Ok) : res.code);
        body.int32(2, res.ok ? res.status : 0);
        body.int32(3, point_id);
        if (res.ok) {
            proto::Writer reward;
            for (const auto& [item_id, num] : res.shown) {
                proto::Writer entry;
                entry.int32(1, item_id);
                entry.int32(2, num);
                reward.message(1, entry.data());
            }
            body.message(4, reward.data());
        }
        const auto reply = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_AchvPointReward),
                                                    frame.head.request_id, frame.head.session_id, data_version_,
                                                    static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
        out.insert(out.end(), reply.begin(), reply.end());
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_MedalOpt)) {
        // 411 → 412: 保存主页徽章展示槽。{repeated f1 KeyValuePair{1:槽0..2, 2:medalId(0=摘下)}}
        // 客户端 SendModuleSelect(0x0130FD00) 按选择顺序生成 Key=i; OnReceiveMedalOptMsg
        // 只弹气泡+事件194 刷新, 数据全靠回包前的 PlayerData 推送 (顺序不可反)。
        std::vector<std::pair<std::int32_t, std::int32_t>> slots;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->wire_type == 2 && field->number == 1) {
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
                slots.emplace_back(slot, id);
            } else if (!reader.skip(field->wire_type)) {
                break;
            }
        }
        std::int32_t code = static_cast<std::int32_t>(LogicCode::Ok);
        bool valid = !slots.empty() && slots.size() <= 3;
        for (const auto& [slot, id] : slots) {
            if (slot < 0 || slot > 2 || id < 0) valid = false;
            // 未拥有的徽章不可佩戴 (id 0 = 摘下, 恒合法)。
            if (id > 0 && !account_.player.medal_pack.contains(id)) valid = false;
        }
        if (valid) {
            account_.player.medal_show.clear();
            for (const auto& [slot, id] : slots) {
                if (id > 0) account_.player.medal_show[slot] = id;
            }
            accounts_.save(account_key_, account_);
        } else {
            code = 3; // E_ResourceInvalid: 客户端 ShowErrorCodeBubble
        }
        std::vector<std::uint8_t> out;
        if (valid) out = push_player_data(frame);
        proto::Writer body; // L2C_MedalOpt: 1 code
        body.int32(1, code);
        const auto reply = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_MedalOpt),
                                                    frame.head.request_id, frame.head.session_id, data_version_,
                                                    static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
        out.insert(out.end(), reply.begin(), reply.end());
        return out;
    }

    // ---- 战斗遥测/活动增益空状态 ----
    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_FightKillInfo)) {
        // 316 → 318: 每英雄击杀统计 + 章节任务事件, 仅遥测。L2C 只有 {1: code}。
        proto::Writer body;
        body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_FightKillInfo),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
    }
    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_AccountBuffData) ||
        frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_AccountBuffAutoStop)) {
        // 865/885 → 866/886: 账号级限时增益 (GodSoonModule, 双倍掉落卡等)。
        // 离线恒无增益; 账号内本就不存在 buff 表数据, 回 code=10 空态。
        // 866 = {1: code, 2: repeated AccountBuffData, 3: repeated buffId};
        // 886 = {1: code, 2: repeated AccountBuffData}。
        const auto reply_id = frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_AccountBuffData)
                                  ? static_cast<std::uint32_t>(ProtoId::L2C_AccountBuffData)
                                  : static_cast<std::uint32_t>(ProtoId::L2C_AccountBuffAutoStop);
        proto::Writer body;
        body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
        return marsnet::encode_response(reply_id, frame.head.request_id, frame.head.session_id,
                                        data_version_, static_cast<std::int32_t>(LogicCode::Ok), "",
                                        body.data());
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_AlchemyMainData)) {
        // MainHallFSM.UpdateLoadModule sends this once Faust's alchemy lab
        // (白夜行星·浮士德的炼金实验室) FunctionOpen is reached.
        // OnReceiveAlchemyMainDataMsg throws on null lists (SilentOrbit only
        // allocates a List once an entry exists) and any real Element entry
        // runs ResetElementSimulation -> CollegeModule.GetCurLevel, which needs
        // growthData from L2C_Login.f14. Entries below are no-op safe:
        // recipe 0 is not in CollegeRecipe (RefreshRecipeMaxUnlock skips),
        // CustomerInfo/ProductionBar with field 1 == 0 become null slots, and
        // elements 991-996 start empty with lastRecoverTime = now.
        proto::Writer body; // L2C_AlchemyMainData: 1 code, 2 recipeIdExp, 3 customeres,
                            // 4 productionBars, 5 elements, 6 buffType, 7 buffCount
        body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
        proto::Writer recipe_exp; // KeyValuePair_Int32_Int32: 1 key, 2 value
        recipe_exp.int32(1, 0);
        recipe_exp.int32(2, 0);
        body.message(2, recipe_exp.data());
        body.message(3, {}); // empty CustomerInfo -> null slot
        body.message(4, {}); // empty ProductionBar -> null slot
        const auto now = offline::client_time(std::time(nullptr));
        for (std::int32_t i = 0; i < 6; ++i) {
            proto::Writer element; // Element: 1 elementId, 2 num, 3 lastRecoverTime, 4 buyTimesDay
            element.int32(1, 991 + i);
            element.int32(2, 0);
            element.int32(3, now);
            body.message(5, element.data());
        }
        body.int32(6, 0);
        body.int32(7, 0);
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_AlchemyMainData),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_QueryHeroDubbing)) {
        // Locked voices only unlock when the server says so: the client has no
        // click-to-unlock path (only the hero talking sends C2L_SaveHeroDubbing).
        // Besides persisted unlocks, grant every Dubbing row whose UnlockConditions
        // text (Language 1350101..1350124) is already satisfied.
        proto::Writer body; // L2C_QueryHeroDubbing: 1 code, 2 repeated HeroDubbingData
        body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
        for (const auto& hero : account_.heroes) {
            const auto ids = offline::dubbing_unlock_ids(hero, tables_.get());
            if (ids.empty()) continue;
            proto::Writer entry; // HeroDubbingData: 1 heroId, 2 repeated dubbingId (varint, unpacked)
            entry.int32(1, hero.id);
            for (const auto dub_id : ids) entry.int32(2, dub_id);
            body.message(2, entry.data());
        }
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_QueryHeroDubbing),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_SaveHeroDubbing)) {
        std::int32_t hero_id = 0, dubbing_id = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->wire_type == 0 && field->number == 1) hero_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->wire_type == 0 && field->number == 2) dubbing_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }

        auto it = std::find_if(account_.heroes.begin(), account_.heroes.end(),
                               [hero_id](const auto& h) { return h.id == hero_id; });
        const bool found = (it != account_.heroes.end() && dubbing_id > 0);
        if (found) {
            if (!std::ranges::contains(it->dubbings, dubbing_id)) it->dubbings.push_back(dubbing_id);
            accounts_.save(account_key_, account_);
        }

        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("save hero dubbing hero={} dubbing={} ok={} body: {}",
                                       hero_id, dubbing_id, found, proto::dump(frame.body)));

        proto::Writer body; // L2C_SaveHeroDubbing: 1 code, 2 repeated HeroDubbingData
        // The client replaces mHeroDubbingData[heroId] wholesale, so the reply
        // must carry the union of persisted and condition-granted voices.
        body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
        proto::Writer entry;
        entry.int32(1, hero_id);
        if (found) {
            for (const auto dub_id : offline::dubbing_unlock_ids(*it, tables_.get())) entry.int32(2, dub_id);
        } else if (dubbing_id > 0) {
            // Unknown hero: stateless echo so the UI still unlocks this session.
            entry.int32(2, dubbing_id);
        } else {
            entry.int32(2, 0);
        }
        body.message(2, entry.data());
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_SaveHeroDubbing),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_UpgradeFetters)) {
        offline::UpgradeFettersRequest req;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->wire_type == 0 && field->number == 1) req.hero_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->wire_type == 0 && field->number == 2) req.position_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }

        const auto res = offline::apply_upgrade_fetters(account_, tables_.get(), req);
        if (res.ok) {
            accounts_.save(account_key_, account_);
        }

        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("upgrade fetters hero={} pos={} ok={} code={} lv={} body: {}",
                                       req.hero_id, req.position_id, res.ok, res.code, res.new_level, proto::dump(frame.body)));

        std::vector<std::uint8_t> out;
        const auto append = [&out](const std::vector<std::uint8_t>& bytes) {
            out.insert(out.end(), bytes.begin(), bytes.end());
        };

        if (res.ok) {
            // 1. 推送 L2C_HeroUpdate (549)
            proto::Writer hero_update;
            hero_update.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
            hero_update.message(2, offline::ProtocolBuilders::hero_data(res.updated_hero, tables_.get()));
            append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_HeroUpdate), 0,
                                            frame.head.session_id, ++data_version_,
                                            static_cast<std::int32_t>(LogicCode::Ok), "",
                                            hero_update.data()));

            // 2. 如果金币变动，推送 PlayerDataProto (1000)
            if (res.gold_changed) {
                append(push_player_data(frame));
            }
        }

        // 3. 应答 L2C_UpgradeFetters (499)
        // 字段: 1 code, 2 mainHeroId, 3 positionId
        proto::Writer body;
        body.int32(1, res.code);
        body.int32(2, res.hero_id);
        body.int32(3, res.position_id);
        append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_UpgradeFetters),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data()));
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_FavorBreak)) {
        offline::FavorBreakRequest req;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->wire_type == 0 && field->number == 1) req.hero_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }

        const auto res = offline::apply_favor_break(account_, tables_.get(), req);
        if (res.ok) {
            accounts_.save(account_key_, account_);
        }

        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("favor break hero={} ok={} code={} lv={} body: {}",
                                       req.hero_id, res.ok, res.code, res.after_level, proto::dump(frame.body)));

        std::vector<std::uint8_t> out;
        const auto append = [&out](const std::vector<std::uint8_t>& bytes) {
            out.insert(out.end(), bytes.begin(), bytes.end());
        };

        if (res.ok) {
            // 1. 推送消耗道具更新 L2C_ItemUpdate (553)
            for (const auto& item : res.changed_items) {
                proto::Writer item_update;
                item_update.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
                item_update.message(2, offline::ProtocolBuilders::item_data(item));
                append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_ItemUpdate), 0,
                                                frame.head.session_id, ++data_version_,
                                                static_cast<std::int32_t>(LogicCode::Ok), "",
                                                item_update.data()));
            }

            // 2. 推送神格更新 L2C_HeroUpdate (549)
            proto::Writer hero_update;
            hero_update.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
            hero_update.message(2, offline::ProtocolBuilders::hero_data(res.updated_hero, tables_.get()));
            append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_HeroUpdate), 0,
                                            frame.head.session_id, ++data_version_,
                                            static_cast<std::int32_t>(LogicCode::Ok), "",
                                            hero_update.data()));

            // 3. 推送 PlayerDataProto (1000) 确保客户端 FavorMap 全量同步为 5 级
            append(push_player_data(frame));

            // 4. 若突破解锁了新语音，推送 L2C_SaveHeroDubbing (689)
            if (!res.unlocked_dubbings.empty()) {
                proto::Writer dubbing_body;
                dubbing_body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
                proto::Writer dubbing_entry;
                dubbing_entry.int32(1, res.hero_id);
                for (const auto dub_id : res.unlocked_dubbings) dubbing_entry.int32(2, dub_id);
                dubbing_body.message(2, dubbing_entry.data());
                append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_SaveHeroDubbing), 0,
                                                frame.head.session_id, ++data_version_,
                                                static_cast<std::int32_t>(LogicCode::Ok), "",
                                                dubbing_body.data()));
            }
        }

        // 4. 应答 L2C_FavorBreak (652)
        // 字段: 1 code, 2 heroId
        proto::Writer body;
        body.int32(1, res.code);
        body.int32(2, res.hero_id);
        append(marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_FavorBreak),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data()));
        return out;
    }

    if (tables_ && (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_QueryActivity) ||
                    frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_CardPool))) {
        const bool activity = frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_QueryActivity);
        return marsnet::encode_response(
            static_cast<std::uint32_t>(activity ? ProtoId::L2C_QueryActivity : ProtoId::L2C_CardPool),
            frame.head.request_id, frame.head.session_id, data_version_, static_cast<std::int32_t>(LogicCode::Ok), "",
            activity ? offline::ProtocolBuilders::query_activity(*tables_, current_time())
                     : offline::ProtocolBuilders::card_pool(*tables_, account_, current_time()));
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_ServerTableConfig)) {
        // L2C_ServerTableConfig{code, keyVal}. The keyVal list MUST be present
        // with at least one entry: the field is null when omitted and
        // MainModule::InitServerTableData throws on a null list, which drops
        // the connection ("初始化数据出错"). One empty pair is harmless — the
        // loop matches known keys (PowerBuyNum, ...) and ignores the rest.
        proto::Writer body;
        body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
        std::vector<std::uint8_t> pair;
        {
            proto::Writer inner;
            inner.string(1, "");
            inner.string(2, "");
            pair = inner.data();
        }
        body.message(2, pair);
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_ServerTableConfig),
                                         frame.head.request_id, frame.head.session_id, 1,
                                         static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_ReceiveAttachment) ||
        frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_ReceiveAllAttachment)) {
        // Single claim and claim-all both send 208 in this client: f1 mail ids,
        // f2 the RewardData it already parsed. 197 is the one-id form.
        const bool claim_all = frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_ReceiveAllAttachment);
        std::vector<std::int64_t> mail_ids;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->number == 1 && field->wire_type == 0) {
                mail_ids.push_back(static_cast<std::int64_t>(reader.varint().value_or(0)));
            } else if (!reader.skip(field->wire_type)) {
                break;
            }
        }
        std::vector<offline::MailClaim> claimed;
        bool player_pushed = false;
        std::vector<offline::AccountItem> bag_updates;
        std::vector<offline::AccountEquip> equip_updates;
        std::vector<offline::AccountItem> all_shown;

        for (const auto mail_id : mail_ids) {
            if (auto claim = offline::claim_mail(account_, mail_id, tables_.get()); claim.ok) {
                if (claim.player) player_pushed = true;
                for (const auto& b : claim.bag_items) {
                    auto it = std::ranges::find(bag_updates, b.id, &offline::AccountItem::id);
                    if (it != bag_updates.end()) *it = b;
                    else bag_updates.push_back(b);
                }
                equip_updates.insert(equip_updates.end(), claim.equips.begin(), claim.equips.end());
                for (const auto& s : claim.shown) {
                    auto it = std::ranges::find(all_shown, s.id, &offline::AccountItem::id);
                    if (it != all_shown.end()) it->num += s.num;
                    else all_shown.push_back(s);
                }
                claimed.push_back(std::move(claim));
            }
        }
        std::vector<std::uint8_t> out;
        if (!claimed.empty()) {
            accounts_.save(account_key_, account_);
            if (player_pushed) {
                const auto p = push_player_data(frame);
                out.insert(out.end(), p.begin(), p.end());
            }
            if (!bag_updates.empty()) {
                proto::Writer update;
                update.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
                for (const auto& item : bag_updates) {
                    update.message(2, offline::ProtocolBuilders::item_data(item));
                }
                const auto pushed = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_ItemUpdate), 0,
                                                             frame.head.session_id, ++data_version_,
                                                             static_cast<std::int32_t>(LogicCode::Ok), "", update.data());
                out.insert(out.end(), pushed.begin(), pushed.end());
            }
            if (!equip_updates.empty()) {
                proto::Writer update;
                update.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
                for (const auto& equip : equip_updates) {
                    update.message(2, offline::ProtocolBuilders::hero_equip(equip));
                }
                const auto pushed = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_EquipUpdate), 0,
                                                             frame.head.session_id, ++data_version_,
                                                             static_cast<std::int32_t>(LogicCode::Ok), "", update.data());
                out.insert(out.end(), pushed.begin(), pushed.end());
            }
        }
        proto::Writer body;
        body.int32(1, static_cast<std::int32_t>(claimed.empty() ? LogicCode::ErrorOpt : LogicCode::Ok));
        if (!claimed.empty()) {
            proto::Writer reward;
            for (const auto& item : all_shown) {
                proto::Writer entry;
                entry.int32(1, item.id);
                entry.int32(2, item.num);
                reward.message(1, entry.data());
            }
            for (const auto& equip : equip_updates) {
                reward.message(2, offline::ProtocolBuilders::hero_equip(equip));
            }
            body.message(2, reward.data());
        }
        // L2C_ReceiveAttachment.mailid is field 3. L2C_ReceiveAllAttachment.mailids
        // is also field 3 (code, rewardData, ids).
        for (const auto mail_id : mail_ids) {
            if (claimed.empty()) break;
            body.int64(3, mail_id);
        }
        const auto reply_id = claim_all ? ProtoId::L2C_ReceiveAllAttachment : ProtoId::L2C_ReceiveAttachment;
        const auto reply = marsnet::encode_response(static_cast<std::uint32_t>(reply_id), frame.head.request_id,
                                                    frame.head.session_id, data_version_,
                                                    static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
        out.insert(out.end(), reply.begin(), reply.end());
        return out;

    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_QueryCollectionAward)) {
        proto::Writer body; // L2C_QueryCollectionAward: repeated award ids already taken
        for (const auto id : account_.player.collection_awards) body.int32(1, id);
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_QueryCollectionAward),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_GetCollectionAward)) {
        std::int32_t award_id = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->number == 1 && field->wire_type == 0) award_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }
        const auto claim = offline::claim_collection_award(account_, tables_.get(), award_id);
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("collection award={} ok={} already={} body: {}", award_id, claim.ok, claim.already,
                                       proto::dump(frame.body)));
        std::vector<std::uint8_t> out;
        if (claim.ok && !claim.already) {
            accounts_.save(account_key_, account_);
            if (claim.player) out = push_player_data(frame);
            if (!claim.bag.empty()) {
                proto::Writer update;
                update.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
                for (const auto& item : claim.bag) update.message(2, offline::ProtocolBuilders::item_data(item));
                const auto pushed = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_ItemUpdate), 0,
                                                             frame.head.session_id, ++data_version_,
                                                             static_cast<std::int32_t>(LogicCode::Ok), "", update.data());
                out.insert(out.end(), pushed.begin(), pushed.end());
            }
        }
        proto::Writer body; // L2C_GetCollectiontAward: 1 code, 2 repeated awardID,
                            // 3 rewardData. OnHandleGetCollectAward REPLACES its whole
                            // claimed list with field 2 (null list throws), so it must
                            // carry the full updated collection_awards — a single id
                            // collapses the client list to one entry and every other
                            // claimed chest re-shows as claimable.
        body.int32(1, static_cast<std::int32_t>(claim.ok ? LogicCode::Ok : LogicCode::ErrorOpt));
        if (claim.ok) {
            for (const auto id : account_.player.collection_awards) body.int32(2, id);
            proto::Writer reward; // RewardData: 1 repeated RewardItem{1 itemId, 2 itemNum}
            for (const auto& item : claim.shown) {
                proto::Writer entry;
                entry.int32(1, item.id);
                entry.int32(2, item.num);
                reward.message(1, entry.data());
            }
            body.message(3, reward.data()); // present even when empty: field 3 goes
                                            // straight into ShowGetAwardItem unguarded
        }
        const auto reply = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_GetCollectionAward),
                                                    frame.head.request_id, frame.head.session_id, data_version_,
                                                    static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
        out.insert(out.end(), reply.begin(), reply.end());
        return out;
    }

    if (tables_ && frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_GameTask)) {
        std::int32_t type = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->number == 1 && field->wire_type == 0) type = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }
        if (type == 1 || type == 3) { // DAILY or CHALLENGE. Other types stay on the generic reply.
            const auto body = type == 3 ? offline::challenge_task_reply(account_, *tables_)
                                        : offline::daily_task_reply(account_, *tables_);
            return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_GameTask), frame.head.request_id,
                                            frame.head.session_id, data_version_,
                                            static_cast<std::int32_t>(LogicCode::Ok), "", body);
        }
    }

    if (tables_ && frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_FinishGameTask)) {
        struct Req { std::int32_t id{}; std::int32_t type{}; };
        std::vector<Req> reqs;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->number == 1 && field->wire_type == 2) {
                Req req;
                proto::Reader entry{reader.bytes().value_or(std::span<const std::uint8_t>{})};
                while (!entry.eof()) {
                    const auto inner = entry.field();
                    if (!inner) break;
                    if (inner->wire_type == 0 && inner->number == 1) req.id = static_cast<std::int32_t>(entry.varint().value_or(0));
                    else if (inner->wire_type == 0 && inner->number == 2) req.type = static_cast<std::int32_t>(entry.varint().value_or(0));
                    else if (!entry.skip(inner->wire_type)) break;
                }
                reqs.push_back(req);
            } else if (!reader.skip(field->wire_type)) {
                break;
            }
        }
        struct Done {
            std::int32_t id{};
            std::int32_t type{};
            offline::TaskClaim claim;
        };
        std::vector<Done> done;
        bool player = false;
        std::int32_t ups = 0;
        std::vector<offline::AccountItem> bag;
        bool changed = false;
        for (const auto& req : reqs) {
            if (req.type != 0 && req.type != 1 && req.type != 3) continue;
            auto claim = req.type == 3 ? offline::claim_challenge_task(account_, tables_.get(), req.id)
                                       : offline::claim_daily_task(account_, tables_.get(), req.id);
            player = player || claim.player;
            ups += claim.level_ups;
            changed = changed || (claim.ok && !claim.already);
            bag.insert(bag.end(), claim.bag.begin(), claim.bag.end());
            done.push_back(Done{.id = req.id, .type = req.type == 0 ? 1 : req.type, .claim = std::move(claim)});
        }
        if (done.empty()) {
            // Not a daily claim (favor / weekly). Leave it to the generic reply.
        } else {
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("finish daily n={} changed={} level+{} body: {}", done.size(), changed, ups,
                                       proto::dump(frame.body)));
        std::vector<std::uint8_t> out;
        if (changed) {
            accounts_.save(account_key_, account_);
            if (player) out = push_player_data(frame);
            if (ups > 0) {
                const auto level = push_level_up(frame, account_.player.level - ups, account_.player.level);
                out.insert(out.end(), level.begin(), level.end());
            }
            if (!bag.empty()) {
                proto::Writer update;
                update.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
                for (const auto& item : bag) update.message(2, offline::ProtocolBuilders::item_data(item));
                const auto pushed = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_ItemUpdate), 0,
                                                             frame.head.session_id, ++data_version_,
                                                             static_cast<std::int32_t>(LogicCode::Ok), "", update.data());
                out.insert(out.end(), pushed.begin(), pushed.end());
            }
        }
        proto::Writer body; // L2C_FinishGameTask: repeated RspFinishTaskData
        for (const auto& item : done) {
            proto::Writer one;
            one.int32(1, static_cast<std::int32_t>(item.claim.ok ? LogicCode::Ok : LogicCode::ErrorOpt));
            one.int32(2, item.id);
            if (!item.claim.shown.empty()) {
                proto::Writer reward;
                for (const auto& shown : item.claim.shown) {
                    proto::Writer entry;
                    entry.int32(1, shown.id);
                    entry.int32(2, shown.num);
                    reward.message(1, entry.data());
                }
                one.message(3, reward.data());
            }
            one.int32(4, item.type);
            body.message(1, one.data());
        }
        const auto reply = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_FinishGameTask),
                                                    frame.head.request_id, frame.head.session_id, data_version_,
                                                    static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
        out.insert(out.end(), reply.begin(), reply.end());
        return out;
        }
    }

    if (tables_ && frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_PickTreasureBox)) {
        std::int32_t box_id = 0, type = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->wire_type == 0 && field->number == 1) box_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->wire_type == 0 && field->number == 2) type = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }
        if (type == 0 || type == 1 || type == 3) {
            // C2L_PickTreasureBox for challenge (type 3) sends mChallengeIndex (0-based: 0..9).
            // ChallengeTask groups and pick_challenge_box use 1-based phase (1..10).
            const auto phase = type == 3 ? box_id + 1 : box_id;
            const auto claim = type == 3 ? offline::pick_challenge_box(account_, tables_.get(), phase)
                                         : offline::pick_daily_box(account_, tables_.get(), box_id);
            x2::core::log_line(x2::core::LogLevel::Info, tag,
                               std::format("box={} phase={} type={} ok={} already={} level+{} body: {}", box_id,
                                           phase, type, claim.ok, claim.already, claim.level_ups, proto::dump(frame.body)));
            std::vector<std::uint8_t> out;
            if (claim.ok && !claim.already) {
                accounts_.save(account_key_, account_);
                if (claim.player) out = push_player_data(frame);
                if (claim.level_ups > 0) {
                    const auto level = push_level_up(frame, account_.player.level - claim.level_ups, account_.player.level);
                    out.insert(out.end(), level.begin(), level.end());
                }
                if (!claim.bag.empty()) {
                    proto::Writer update;
                    update.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
                    for (const auto& item : claim.bag) update.message(2, offline::ProtocolBuilders::item_data(item));
                    const auto pushed = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_ItemUpdate), 0,
                                                                 frame.head.session_id, ++data_version_,
                                                                 static_cast<std::int32_t>(LogicCode::Ok), "", update.data());
                    out.insert(out.end(), pushed.begin(), pushed.end());
                }
            }
            proto::Writer body; // L2C_PickTreasureBox: 1 code, 2 boxId, 3 type, 4 rewardData
            body.int32(1, static_cast<std::int32_t>(claim.ok ? LogicCode::Ok : LogicCode::ErrorOpt));
            body.int32(2, box_id);
            body.int32(3, type == 0 ? 1 : type);
            if (!claim.shown.empty()) {
                proto::Writer reward;
                for (const auto& item : claim.shown) {
                    proto::Writer entry;
                    entry.int32(1, item.id);
                    entry.int32(2, item.num);
                    reward.message(1, entry.data());
                }
                body.message(4, reward.data());
            }
            const auto reply = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_PickTreasureBox),
                                                        frame.head.request_id, frame.head.session_id, data_version_,
                                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
            out.insert(out.end(), reply.begin(), reply.end());
            return out;
        }
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_EquipStrengthen)) {
        std::int32_t equip_id = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->wire_type == 0 && field->number == 1) equip_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }
        const auto res = offline::apply_equip_strengthen(account_, tables_.get(), equip_id);
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("equip strengthen id={} ok={} code={} new_level={} body: {}",
                                       equip_id, res.ok, res.code, res.new_level, proto::dump(frame.body)));
        std::vector<std::uint8_t> out;
        if (res.ok) {
            accounts_.save(account_key_, account_);
            // 铁律推送顺序 1: 主动推送货币与兽魂更新 (Proto 1000 PlayerDataProto)
            auto pushed_player = push_player_data(frame);
            out.insert(out.end(), pushed_player.begin(), pushed_player.end());

            // 铁律推送顺序 2: 主动推送兽主属性更新 (Proto 536 L2C_EquipUpdate)
            proto::Writer update;
            update.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
            update.message(2, offline::ProtocolBuilders::hero_equip(res.updated_equip));
            const auto pushed_equip = marsnet::encode_response(
                static_cast<std::uint32_t>(ProtoId::L2C_EquipUpdate), 0,
                frame.head.session_id, ++data_version_,
                static_cast<std::int32_t>(LogicCode::Ok), "", update.data());
            out.insert(out.end(), pushed_equip.begin(), pushed_equip.end());
        }
        // 铁律推送顺序 3: 业务回包 (Proto 117 L2C_EquipStrengthen)
        proto::Writer body;
        body.int32(1, res.code);
        body.int32(2, equip_id);
        body.int32(3, res.new_level);
        const auto reply = marsnet::encode_response(
            static_cast<std::uint32_t>(ProtoId::L2C_EquipStrengthen),
            frame.head.request_id, frame.head.session_id, data_version_,
            static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
        out.insert(out.end(), reply.begin(), reply.end());
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_DoEquip)) {
        std::int32_t equip_id = 0, hero_id = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->wire_type == 0 && field->number == 1) equip_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->wire_type == 0 && field->number == 2) hero_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }
        const auto wear = offline::wear_equip(account_, tables_.get(), equip_id, hero_id);
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("wear equip={} hero={} ok={} body: {}", equip_id, hero_id, wear.ok,
                                       proto::dump(frame.body)));
        std::vector<std::uint8_t> out;
        if (wear.ok) {
            accounts_.save(account_key_, account_);
            proto::Writer update; // L2C_HeroUpdate so RefreshHeroWearingEquip sees the new slots
            update.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
            for (const auto& hero : wear.heroes)
                update.message(2, offline::ProtocolBuilders::hero_data(hero, tables_.get()));
            const auto pushed = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_HeroUpdate), 0,
                                                         frame.head.session_id, ++data_version_,
                                                         static_cast<std::int32_t>(LogicCode::Ok), "", update.data());
            out.insert(out.end(), pushed.begin(), pushed.end());
        }
        proto::Writer body; // L2C_DoEquip: 1 code, 2 equipID, 3 heroID
        body.int32(1, static_cast<std::int32_t>(wear.ok ? LogicCode::Ok : LogicCode::ErrorOpt));
        if (wear.ok) {
            body.int32(2, equip_id);
            body.int32(3, hero_id);
        }
        const auto reply = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_DoEquip),
                                                    frame.head.request_id, frame.head.session_id, data_version_,
                                                    static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
        out.insert(out.end(), reply.begin(), reply.end());
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_DoUnEquip)) {
        std::int32_t pos = 0, hero_id = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->wire_type == 0 && field->number == 1) {
                pos = static_cast<std::int32_t>(static_cast<std::int64_t>(reader.varint().value_or(0)));
            } else if (field->wire_type == 0 && field->number == 2) {
                hero_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            } else if (!reader.skip(field->wire_type)) {
                break;
            }
        }
        const auto wear = offline::unequip(account_, hero_id, pos);
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("unequip hero={} pos={} ok={} body: {}", hero_id, pos, wear.ok,
                                       proto::dump(frame.body)));
        std::vector<std::uint8_t> out;
        if (wear.ok) {
            accounts_.save(account_key_, account_);
            proto::Writer update;
            update.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
            for (const auto& hero : wear.heroes)
                update.message(2, offline::ProtocolBuilders::hero_data(hero, tables_.get()));
            const auto pushed = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_HeroUpdate), 0,
                                                         frame.head.session_id, ++data_version_,
                                                         static_cast<std::int32_t>(LogicCode::Ok), "", update.data());
            out.insert(out.end(), pushed.begin(), pushed.end());
        }
        proto::Writer body; // L2C_DoUnEquip: 1 code, 2 posIdx, 3 heroID
        body.int32(1, static_cast<std::int32_t>(wear.ok ? LogicCode::Ok : LogicCode::ErrorOpt));
        if (wear.ok) {
            body.int32(2, pos);
            body.int32(3, hero_id);
        }
        const auto reply = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_DoUnEquip),
                                                    frame.head.request_id, frame.head.session_id, data_version_,
                                                    static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
        out.insert(out.end(), reply.begin(), reply.end());
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_UpdateEquipPlan)) {
        std::int32_t plan_id = 0;
        std::string name;
        std::map<std::int32_t, std::int32_t> positions;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->number == 1 && field->wire_type == 0) plan_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->number == 2 && field->wire_type == 2) name = reader.string().value_or("");
            else if (field->number == 3 && field->wire_type == 2) {
                proto::Reader pos{reader.bytes().value_or(std::span<const std::uint8_t>{})};
                std::int32_t slot = 0, equip_id = 0;
                while (!pos.eof()) {
                    const auto inner = pos.field();
                    if (!inner || inner->wire_type != 0) break;
                    const auto value = static_cast<std::int32_t>(pos.varint().value_or(0));
                    if (inner->number == 1) slot = value;
                    else if (inner->number == 2) equip_id = value;
                }
                if (equip_id > 0) positions[slot] = equip_id;
            } else if (!reader.skip(field->wire_type)) {
                break;
            }
        }
        plan_id = offline::save_equip_plan(account_, plan_id, std::move(name), std::move(positions));
        accounts_.save(account_key_, account_);
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("equip plan id={} body: {}", plan_id, proto::dump(frame.body)));
        auto out = push_player_data(frame);
        proto::Writer body; // L2C_UpdateEquipPlan: 1 code, 2 planId
        body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
        body.int32(2, plan_id);
        const auto reply = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_UpdateEquipPlan),
                                                    frame.head.request_id, frame.head.session_id, data_version_,
                                                    static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
        out.insert(out.end(), reply.begin(), reply.end());
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_DelEquipPlan)) {
        std::int32_t plan_id = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->number == 1 && field->wire_type == 0) plan_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }
        const auto ok = offline::delete_equip_plan(account_, plan_id);
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("del equip plan id={} ok={} body: {}", plan_id, ok, proto::dump(frame.body)));
        std::vector<std::uint8_t> out;
        if (ok) {
            accounts_.save(account_key_, account_);
            out = push_player_data(frame);
        }
        proto::Writer body; // L2C_DelEquipPlan: 1 code, 2 planId
        body.int32(1, static_cast<std::int32_t>(ok ? LogicCode::Ok : LogicCode::ErrorOpt));
        if (ok) body.int32(2, plan_id);
        const auto reply = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_DelEquipPlan),
                                                    frame.head.request_id, frame.head.session_id, data_version_,
                                                    static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
        out.insert(out.end(), reply.begin(), reply.end());
        return out;
    }

    if (tables_ && frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_StarSkillUp)) {
        std::int32_t skill_id = 0, target = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->wire_type == 0 && field->number == 1) skill_id = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->wire_type == 0 && field->number == 2) target = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }
        const auto up = offline::upgrade_star_skill(account_, tables_.get(), skill_id, target);
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("star skill={} target={} ok={} body: {}", skill_id, target, up.ok,
                                       proto::dump(frame.body)));
        std::vector<std::uint8_t> out;
        if (up.ok) {
            accounts_.save(account_key_, account_);
            // OnStarSkillUp fires event 201, whose listener is StartPowerTimer.
            out = push_growth(frame);
            const auto player = push_player_data(frame);
            out.insert(out.end(), player.begin(), player.end());
        }
        proto::Writer body; // L2C_StarSkillUp: 1 code, 2 skillID, 3 targetLevel
        body.int32(1, static_cast<std::int32_t>(up.ok ? LogicCode::Ok : LogicCode::ErrorOpt));
        if (up.ok) {
            body.int32(2, skill_id);
            body.int32(3, target);
        }
        const auto reply = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_StarSkillUp),
                                                    frame.head.request_id, frame.head.session_id, data_version_,
                                                    static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
        out.insert(out.end(), reply.begin(), reply.end());
        return out;
    }

    auto append_reward = [](proto::Writer& body, std::uint32_t field, const std::vector<offline::AccountItem>& items,
                            const std::vector<offline::AccountEquip>& equips = {}) {
        if (items.empty() && equips.empty()) return;
        proto::Writer reward; // RewardData: 1 rewardItem, 2 rewardEquip
        for (const auto& item : items) {
            proto::Writer entry;
            entry.int32(1, item.id);
            entry.int32(2, item.num);
            reward.message(1, entry.data());
        }
        for (const auto& equip : equips) reward.message(2, offline::ProtocolBuilders::hero_equip(equip));
        body.message(field, reward.data());
    };
    auto push_equips = [&](std::vector<std::uint8_t>& out, const std::vector<offline::AccountEquip>& equips) {
        if (equips.empty()) return;
        proto::Writer update; // L2C_EquipUpdate: 1 code, 2 repeated HeroEquip
        update.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
        for (const auto& equip : equips) update.message(2, offline::ProtocolBuilders::hero_equip(equip));
        const auto pushed = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_EquipUpdate), 0,
                                                     frame.head.session_id, ++data_version_,
                                                     static_cast<std::int32_t>(LogicCode::Ok), "", update.data());
        out.insert(out.end(), pushed.begin(), pushed.end());
    };
    auto push_bag = [&](std::vector<std::uint8_t>& out, const std::vector<offline::AccountItem>& bag) {
        if (bag.empty()) return;
        proto::Writer update;
        update.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
        for (const auto& item : bag) update.message(2, offline::ProtocolBuilders::item_data(item));
        const auto pushed = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_ItemUpdate), 0,
                                                     frame.head.session_id, ++data_version_,
                                                     static_cast<std::int32_t>(LogicCode::Ok), "", update.data());
        out.insert(out.end(), pushed.begin(), pushed.end());
    };

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_SignInReward)) {
        std::int32_t index = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->number == 1 && field->wire_type == 0) index = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }
        const auto claim = offline::claim_rookie_sign(account_, tables_.get(), index, current_time());
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("rookie sign index={} ok={} already={}", index, claim.ok, claim.already));
        std::vector<std::uint8_t> out;
        if (claim.ok) {
            accounts_.save(account_key_, account_);
            out = push_player_data(frame);
            push_bag(out, claim.bag);
        }
        proto::Writer body; // L2C_SignInReward: 1 code, 2 SignCount, 3 rewardData
        body.int32(1, static_cast<std::int32_t>(claim.ok ? LogicCode::Ok : LogicCode::ErrorOpt));
        if (claim.ok) {
            body.int32(2, account_.player.sign_count);
            append_reward(body, 3, claim.shown);
        }
        const auto reply = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_SignInReward),
                                                    frame.head.request_id, frame.head.session_id, data_version_,
                                                    static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
        out.insert(out.end(), reply.begin(), reply.end());
        return out;
    }

    // An empty reply syncs ServerTime to 0 (2018-01-01): RookieSignModule then
    // flags 七日签到 OutDate, hides its tab and still auto-opens 福利 on every MainPage.
    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_SystemInfo)) {
        proto::Writer body; // L2C_SystemInfo: 1 code, 2 serverTime
        body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
        body.int32(2, offline::client_time(current_time()));
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_SystemInfo), frame.head.request_id,
                                        frame.head.session_id, data_version_, static_cast<std::int32_t>(LogicCode::Ok),
                                        "", body.data());
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_QueryDivination)) {
        const auto view = offline::divination_view(account_, tables_.get(), current_time());
        accounts_.save(account_key_, account_);
        proto::Writer body; // 1 id, 3 checkIn, 4 lastDivinationTime, 5 validDate
        body.int32(1, view.id);
        body.int32(3, view.check_in);
        if (account_.player.divination_time > 0) body.int32(4, account_.player.divination_time);
        body.int32(5, view.valid);
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_QueryDivination),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_Divination)) {
        const auto claim = offline::claim_divination(account_, tables_.get(), current_time());
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("divination ok={} already={}", claim.ok, claim.already));
        std::vector<std::uint8_t> out;
        if (claim.ok) {
            accounts_.save(account_key_, account_);
            if (claim.player) out = push_player_data(frame);
            push_bag(out, claim.bag);
        }
        proto::Writer body; // 1 code, 2 rewardData. 101 is already signed today.
        body.int32(1, claim.ok ? static_cast<std::int32_t>(LogicCode::Ok) : claim.already ? 101 : static_cast<std::int32_t>(LogicCode::ErrorOpt));
        if (claim.ok) append_reward(body, 2, claim.shown);
        const auto reply = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_Divination),
                                                    frame.head.request_id, frame.head.session_id, data_version_,
                                                    static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
        out.insert(out.end(), reply.begin(), reply.end());
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_LuckDraw)) {
        std::int32_t pool_id = 0, draw_type = 0; // C2L_LuckDraw: 1 drawnId, 2 drawType
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->wire_type == 0 && (field->number == 1 || field->number == 2)) {
                (field->number == 1 ? pool_id : draw_type) = static_cast<std::int32_t>(reader.varint().value_or(0));
            } else if (!reader.skip(field->wire_type)) {
                break;
            }
        }
        const auto draw = offline::luck_draw(account_, tables_.get(), pool_id, draw_type);
        x2::core::log_line(x2::core::LogLevel::Info, tag,
                           std::format("luck draw pool={} type={} ok={} cards={} new_heroes={}", pool_id, draw_type,
                                       draw.ok, draw.cards.size(), draw.heroes.size()));
        std::vector<std::uint8_t> out;
        if (draw.ok) {
            accounts_.save(account_key_, account_);
            out = push_player_data(frame);
            push_bag(out, draw.bag);
            if (!draw.heroes.empty()) {
                proto::Writer update; // L2C_HeroUpdate: 1 code, 2 repeated HeroData
                update.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
                for (const auto& hero : draw.heroes) update.message(2, offline::ProtocolBuilders::hero_data(hero, tables_.get()));
                const auto pushed = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_HeroUpdate), 0,
                                                             frame.head.session_id, ++data_version_,
                                                             static_cast<std::int32_t>(LogicCode::Ok), "", update.data());
                out.insert(out.end(), pushed.begin(), pushed.end());
            }
        }
        // L2C_LuckDraw: 1 code, 2 drawnId, 3 rewardData, 5 oneDrawCount, 6 tenDrawCount, 8 securityNum.
        // 13 (E_ErrorOpt) makes OnReceiveLuckDrawMsg return to the hall with a code box.
        proto::Writer body;
        body.int32(1, static_cast<std::int32_t>(draw.ok ? LogicCode::Ok : LogicCode::ErrorOpt));
        body.int32(2, pool_id);
        if (draw.ok) {
            proto::Writer reward; // RewardData: 1 rewardItem{1 itemId, 2 itemNum, 3 transform}, 3 transformHero{1 heroId, 2 transform}
            for (const auto& card : draw.cards) {
                proto::Writer entry;
                entry.int32(1, card.item_id);
                entry.int32(2, card.num);
                if (card.transform) entry.boolean(3, true);
                reward.message(1, entry.data());
            }
            for (const auto& card : draw.cards) {
                if (card.hero_id <= 0) continue;
                proto::Writer entry;
                entry.int32(1, card.hero_id);
                if (card.transform) entry.boolean(2, true);
                reward.message(3, entry.data());
            }
            body.message(3, reward.data());
            const auto& state = account_.player.draw_pools[pool_id];
            body.int32(5, state.one);
            body.int32(6, state.ten);
            body.int32(8, state.since_top);
        }
        const auto reply = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_LuckDraw),
                                                    frame.head.request_id, frame.head.session_id, data_version_,
                                                    static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
        out.insert(out.end(), reply.begin(), reply.end());
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_QueryStarPrivilegeReward) ||
        frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_ReciveStarPrivilegeReward)) {
        const bool claim_request = frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_ReciveStarPrivilegeReward);
        offline::TaskClaim claim;
        std::vector<std::uint8_t> out;
        if (claim_request) {
            std::vector<std::int32_t> levels; // C2L_ReciveStarPrivilegeReward: 1 reciveLevelList
            proto::Reader reader{frame.body};
            while (!reader.eof()) {
                const auto field = reader.field();
                if (!field) break;
                if (field->number == 1 && field->wire_type == 0) {
                    levels.push_back(static_cast<std::int32_t>(reader.varint().value_or(0)));
                } else if (field->number == 1 && field->wire_type == 2) {
                    proto::Reader packed{reader.bytes().value_or(std::span<const std::uint8_t>{})};
                    while (!packed.eof()) levels.push_back(static_cast<std::int32_t>(packed.varint().value_or(0)));
                } else if (!reader.skip(field->wire_type)) {
                    break;
                }
            }
            claim = offline::claim_course(account_, tables_.get(), levels);
            x2::core::log_line(x2::core::LogLevel::Info, tag,
                               std::format("special course levels={} ok={} shown={}", levels.size(), claim.ok, claim.shown.size()));
            if (claim.ok) {
                accounts_.save(account_key_, account_);
                if (claim.player || claim.level_ups > 0) out = push_player_data(frame);
                push_bag(out, claim.bag);
                push_equips(out, claim.equips);
            }
        }
        // 1 code, 2 privilegeRewardList{1 level, 2 reciveFreeReward, 3 recivePrivilegeReward}, 3 rewardData.
        // A level missing from the list reads as claimable once reached.
        proto::Writer body;
        body.int32(1, static_cast<std::int32_t>(!claim_request || claim.ok ? LogicCode::Ok : LogicCode::ErrorOpt));
        for (const auto level : account_.player.course_levels) {
            proto::Writer entry;
            entry.int32(1, level);
            entry.int32(2, 1);
            body.message(2, entry.data());
        }
        if (claim.ok) append_reward(body, 3, claim.shown, claim.equips);
        const auto reply = marsnet::encode_response(
            static_cast<std::uint32_t>(claim_request ? ProtoId::L2C_ReciveStarPrivilegeReward
                                                     : ProtoId::L2C_QueryStarPrivilegeReward),
            frame.head.request_id, frame.head.session_id, data_version_, static_cast<std::int32_t>(LogicCode::Ok), "",
            body.data());
        out.insert(out.end(), reply.begin(), reply.end());
        return out;
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_ItemOpt)) {
        // C2L_ItemOpt: 1 id, 2 opt, 3 count, 4 selectedItemIndexList
        std::int32_t id = 0, opt = 0, count = 0;
        std::vector<std::int32_t> picks;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->wire_type == 0 && field->number <= 4) {
                const auto value = static_cast<std::int32_t>(reader.varint().value_or(0));
                if (field->number == 1) id = value;
                else if (field->number == 2) opt = value;
                else if (field->number == 3) count = value;
                else picks.push_back(value);
            } else if (field->wire_type == 2 && field->number == 4) {
                proto::Reader packed{reader.bytes().value_or(std::span<const std::uint8_t>{})};
                while (!packed.eof()) picks.push_back(static_cast<std::int32_t>(packed.varint().value_or(0)));
            } else if (!reader.skip(field->wire_type)) {
                break;
            }
        }
        if (opt == 0) {
            const auto claim = offline::use_item(account_, tables_.get(), id, count, picks);
            x2::core::log_line(x2::core::LogLevel::Info, tag,
                               std::format("item use id={} count={} ok={} shown={}", id, count, claim.ok, claim.shown.size()));
            std::vector<std::uint8_t> out;
            if (claim.ok) {
                accounts_.save(account_key_, account_);
                if (claim.player || claim.level_ups > 0) out = push_player_data(frame);
                std::vector<offline::AccountItem> kept;
                proto::Writer removed; // L2C_ItemRemove.ids
                for (const auto& item : claim.bag) {
                    if (item.num > 0) kept.push_back(item);
                    else removed.int32(1, item.id);
                }
                push_bag(out, kept);
                push_equips(out, claim.equips);
                if (!removed.data().empty()) {
                    const auto pushed = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_ItemRemove), 0,
                                                                 frame.head.session_id, ++data_version_,
                                                                 static_cast<std::int32_t>(LogicCode::Ok), "", removed.data());
                    out.insert(out.end(), pushed.begin(), pushed.end());
                }
            }
            proto::Writer body; // L2C_ItemOpt: 1 code, 2 opt, 3 rewardData, 4 itemId
            body.int32(1, static_cast<std::int32_t>(claim.ok ? LogicCode::Ok : LogicCode::ErrorOpt));
            body.int32(2, opt);
            if (claim.ok) append_reward(body, 3, claim.shown, claim.equips);
            body.int32(4, id);
            const auto reply = marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_ItemOpt),
                                                        frame.head.request_id, frame.head.session_id, data_version_,
                                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
            out.insert(out.end(), reply.begin(), reply.end());
            return out;
        }
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_FillBirthday)) {
        std::int32_t month = 0, day = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->number == 1 && field->wire_type == 0) month = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (field->number == 2 && field->wire_type == 0) day = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }
        if (month >= 1 && month <= 12 && day >= 1 && day <= 31) {
            account_.player.birthday = month * 100 + day;
            accounts_.save(account_key_, account_);
        }
        proto::Writer body;
        body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_FillBirthday),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_MissSyncData)) {
        if (frame.head.ack_data_version > 0) {
            data_version_ = frame.head.ack_data_version;
        }
        proto::Writer body;
        body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_MissSyncData),
                                        frame.head.request_id, frame.head.session_id, 0,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_ChatJoin)) {
        proto::Writer body; // L2C_ChatJoin: 1 code
        body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_ChatJoin),
                                        frame.head.request_id, frame.head.session_id, 0,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
    }

    if (frame.proto_id == static_cast<std::uint32_t>(ProtoId::C2L_AchvDetialData)) {
        // GetDetailData indexes PrevIndex[achvType - 1]. Code Ok with type 0
        // throws and the net tick drops the socket (the 5→6 连接中).
        std::int32_t achv_type = 0;
        proto::Reader reader{frame.body};
        while (!reader.eof()) {
            const auto field = reader.field();
            if (!field) break;
            if (field->number == 1 && field->wire_type == 0)
                achv_type = static_cast<std::int32_t>(reader.varint().value_or(0));
            else if (!reader.skip(field->wire_type)) break;
        }
        // L2C_AchvDetialData: 1 code, 3 achvType. Code Ok with a null list reads as a full
        // page and requests the next 30 forever; 97 (E_OUT_OF_ACHV_INDEX) ends paging.
        proto::Writer body;
        body.int32(1, 97);
        if (achv_type > 0) body.int32(3, achv_type);
        return marsnet::encode_response(static_cast<std::uint32_t>(ProtoId::L2C_AchvDetialData),
                                        frame.head.request_id, frame.head.session_id, data_version_,
                                        static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
    }

    // Unhandled request: answer with its L2C twin (the client dispatches replies
    // by type) and code=E_Ok where the reply starts with a code field. The raw
    // dump is the request schema evidence (grep the id in memory/protocol_ids.json).
    struct Route {
        std::uint32_t request;
        std::uint32_t reply;
        bool code_first;
    };
    static constexpr Route kRoutes[] = {
#include "server/reply_routes.inc"
    };
    const auto* route = std::ranges::find(kRoutes, frame.proto_id, &Route::request);
    x2::core::log_line(x2::core::LogLevel::Warn, tag,
                       std::format("unhandled proto={} -> {} body: {}", frame.proto_id,
                                   route != std::end(kRoutes) ? route->reply : frame.proto_id,
                                   proto::dump(frame.body)));
    proto::Writer body;
    if (route != std::end(kRoutes) && route->code_first) body.int32(1, static_cast<std::int32_t>(LogicCode::Ok));
    return marsnet::encode_response(route != std::end(kRoutes) ? route->reply : frame.proto_id,
                                     frame.head.request_id, frame.head.session_id, data_version_,
                                     static_cast<std::int32_t>(LogicCode::Ok), "", body.data());
}

} // namespace x2::server
