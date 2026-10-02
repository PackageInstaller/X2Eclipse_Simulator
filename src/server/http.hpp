#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace x2::server {

struct HttpRequest {
    std::string method;
    std::string target;
    std::string path;
    std::map<std::string, std::string> query;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    std::uint32_t status{200};
    std::string content_type{"application/json"};
    std::string body;
    bool keep_alive{true};
};

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    bool listen(std::string_view host, std::uint16_t port);
    void run();
    void route(std::string path, Handler handler);
    void set_default(Handler handler);

private:
    void handle_client(int client);

    int server_fd_{-1};
    std::map<std::string, Handler, std::less<>> routes_;
    Handler default_;
};

} // namespace x2::server
