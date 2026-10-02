#include "persist/account_db.hpp"

#include <cstring>
#include <mutex>
#include <format>
#include <print>

#include <sqlite3.h>
#include <unistd.h>

namespace x2::persist {

namespace {

std::mutex g_mutex;
sqlite3* g_db = nullptr;

std::string package_name() {
    std::string cmd;
    if (FILE* f = fopen("/proc/self/cmdline", "rb")) {
        char buf[256] = {};
        const std::size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        cmd.assign(buf, strnlen(buf, n));
    }
    return cmd;
}

std::vector<std::string> candidate_paths() {
#ifdef __ANDROID__
    const std::string pkg = package_name();
    if (pkg.empty()) return {};
    return {"/data/data/" + pkg + "/files/x2_offline.db", "/data/user/0/" + pkg + "/files/x2_offline.db"};
#else
// 调试
    return {"x2_offline.db"};
#endif
}

void open_database() {
    if (g_db) return;
    for (const auto& path : candidate_paths()) {
        if (sqlite3_open_v2(path.c_str(), &g_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) ==
            SQLITE_OK) {
            char* err = nullptr;
            sqlite3_exec(
                g_db,
                "CREATE TABLE IF NOT EXISTS accounts ("
                " token TEXT PRIMARY KEY,"
                " id INTEGER NOT NULL,"
                " login_count INTEGER NOT NULL,"
                " is_create_role INTEGER NOT NULL,"
                " heroes BLOB NOT NULL,"
                " items BLOB NOT NULL)",
                nullptr, nullptr, &err);
            if (err) {
                std::println("[PERSIST] schema error: {}", err);
                sqlite3_free(err);
            }
            sqlite3_exec(g_db, "ALTER TABLE accounts ADD COLUMN player BLOB NOT NULL DEFAULT x''", nullptr,
                         nullptr, nullptr);
            return;
        }
        g_db = nullptr;
    }
    std::println("[PERSIST] no database opened");
}

} // namespace

bool init_database() {
    std::lock_guard lock{g_mutex};
    open_database();
    return g_db != nullptr;
}

void save_account(const AccountRow& row) {
    std::lock_guard lock{g_mutex};
    if (!g_db) return;
    static sqlite3_stmt* stmt = nullptr;
    if (!stmt) {
        sqlite3_prepare_v2(g_db,
                           "INSERT INTO accounts(token,id,login_count,is_create_role,heroes,items,player)"
                           " VALUES(?1,?2,?3,?4,?5,?6,?7)"
                           " ON CONFLICT(token) DO UPDATE SET id=?2, login_count=?3,"
                           " is_create_role=?4, heroes=?5, items=?6, player=?7",
                           -1, &stmt, nullptr);
    }
    if (!stmt) return;
    sqlite3_reset(stmt);
    sqlite3_bind_text(stmt, 1, row.token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, row.id);
    sqlite3_bind_int(stmt, 3, row.login_count);
    sqlite3_bind_int(stmt, 4, row.is_create_role);
    sqlite3_bind_blob(stmt, 5, row.heroes.data(), static_cast<int>(row.heroes.size()), SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 6, row.items.data(), static_cast<int>(row.items.size()), SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 7, row.player.data(), static_cast<int>(row.player.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::println("[PERSIST] save_account failed: {}", sqlite3_errmsg(g_db));
    }
}

std::vector<AccountRow> load_accounts() {
    std::lock_guard lock{g_mutex};
    std::vector<AccountRow> out;
    if (!g_db) return out;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, "SELECT token,id,login_count,is_create_role,heroes,items,player FROM accounts",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return out;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        AccountRow row;
        const auto* token = sqlite3_column_text(stmt, 0);
        row.token = token ? reinterpret_cast<const char*>(token) : "";
        row.id = sqlite3_column_int64(stmt, 1);
        row.login_count = sqlite3_column_int(stmt, 2);
        row.is_create_role = sqlite3_column_int(stmt, 3);
        const auto* heroes = sqlite3_column_blob(stmt, 4);
        const int heroes_size = sqlite3_column_bytes(stmt, 4);
        const auto* items = sqlite3_column_blob(stmt, 5);
        const int items_size = sqlite3_column_bytes(stmt, 5);
        row.heroes.assign(static_cast<const std::uint8_t*>(heroes),
                          static_cast<const std::uint8_t*>(heroes) + heroes_size);
        row.items.assign(static_cast<const std::uint8_t*>(items),
                         static_cast<const std::uint8_t*>(items) + items_size);
        const auto* player = static_cast<const std::uint8_t*>(sqlite3_column_blob(stmt, 6));
        row.player.assign(player, player + sqlite3_column_bytes(stmt, 6));
        out.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return out;
}

} // namespace x2::persist
