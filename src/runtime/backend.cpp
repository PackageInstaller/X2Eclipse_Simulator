#include "runtime/backend.hpp"

#include <atomic>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <format>
#include <print>
#include <thread>

#include "offline/table_blob.hpp"
#include "persist/account_db.hpp"
#include "server/http.hpp"
#include "server/router.hpp"
#include "server/tcp_server.hpp"

namespace x2::runtime {

namespace {
std::once_flag start_once_;
} // namespace

std::vector<std::uint8_t> read_file(std::string_view path) {
    std::ifstream file{std::string{path}, std::ios::binary};
    if (!file) return {};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

void start(std::span<const std::uint8_t> tables, std::uint16_t http_port, std::uint16_t tcp_port) {
    std::call_once(start_once_, [tables, http_port, tcp_port] {
        auto blob = std::make_shared<offline::TableBlob>();
        if (!tables.empty() && blob->own_blob_and_load(std::vector<std::uint8_t>{tables.begin(), tables.end()})) {
            std::println("[DATA] loaded {} tables", blob->size());
        } else {
            std::println("[DATA] no tables loaded; endpoints still answer");
        }

        auto router = std::make_shared<server::Router>();
        persist::init_database();
        router->set_tables(blob);

        auto http = std::make_shared<server::HttpServer>();
        for (const char* path : {"/apply/httpLogin", "/apply/httpLogin163", "/apply/connectInfo",
                                 "/apply/address", "/apply/controlInfo", "/apply/noticeUrl",
                                 "/apply/loginStep", "/register", "/loginwithpw"}) {
            http->route(path, [router](const x2::server::HttpRequest& request) { return router->http(request); });
        }
        http->set_default([router](const x2::server::HttpRequest& request) { return router->http(request); });

        auto tcp = std::make_shared<server::MarsNetTcpServer>(*router);

        std::thread([http, tcp, http_port] {
            if (!http->listen("127.0.0.1", http_port)) return;
            std::println("[MAIN] HTTP 127.0.0.1:{}", http_port);
            http->run();
        }).detach();

        std::thread([router, tcp, tcp_port] {
            if (!tcp->listen("127.0.0.1", tcp_port)) return;
            std::println("[MAIN] MarsNet TCP 127.0.0.1:{}", tcp_port);
            tcp->run();
        }).detach();
    });
}

void start_from_file(std::string_view tables_path, std::uint16_t http_port, std::uint16_t tcp_port) {
    auto blob = read_file(tables_path);
    start(blob, http_port, tcp_port);
}

} // namespace x2::runtime
