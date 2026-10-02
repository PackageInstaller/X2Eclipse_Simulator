#include "android/net_redirect.hpp"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>

#include <android/log.h>

#define GUM_STATIC
#include "frida-gum.h"

#include "crypto/obf_string.hpp"

namespace x2::android {

namespace {

#define kLogTag (OBFLAZY("x2offline"))

int (*real_connect)(int, const sockaddr*, socklen_t) = nullptr;
int (*real_getaddrinfo)(const char*, const char*, const addrinfo*, addrinfo**) = nullptr;

bool ends_with(const char* s, std::string_view suffix) {
    const std::size_t len = std::strlen(s);
    return len >= suffix.size() && std::memcmp(s + len - suffix.size(), suffix.data(), suffix.size()) == 0;
}

int hook_connect(int fd, const sockaddr* addr, socklen_t len) {
    if (addr != nullptr && addr->sa_family == AF_INET && len >= sizeof(sockaddr_in)) {
        auto* in = reinterpret_cast<const sockaddr_in*>(addr);
        const std::uint32_t ip = ntohl(in->sin_addr.s_addr);
        const std::uint16_t port = ntohs(in->sin_port);
        if ((ip >> 24) == 127 && (port == 80 || port == 443)) {
            auto* writable = const_cast<sockaddr_in*>(in);
            writable->sin_port = htons(9999);
        }
    }
    return real_connect(fd, addr, len);
}

int hook_getaddrinfo(const char* node, const char* service, const addrinfo* hints, addrinfo** res) {
    if (node != nullptr && ends_with(node, OBFCSTR(".17m3.com"))) {
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "redirect %s -> 127.0.0.1", node);
        node = OBFCSTR("127.0.0.1");
    }
    return real_getaddrinfo(node, service, hints, res);
}

void* target_for(const char* symbol) {
    void* handle = dlopen(OBFCSTR("libc.so"), RTLD_NOW | RTLD_NOLOAD);
    if (!handle) handle = dlopen(OBFCSTR("libc.so"), RTLD_NOW);
    if (!handle) return nullptr;
    void* addr = dlsym(handle, symbol);
    dlclose(handle);
    return addr;
}

} // namespace

void install_net_redirect() {
    std::thread([] {
        gum_init_embedded();
        auto* interceptor = gum_interceptor_obtain();

        void* connect_addr = target_for(OBFCSTR("connect"));
        void* gai_addr = target_for(OBFCSTR("getaddrinfo"));
        if (!connect_addr || !gai_addr) {
            __android_log_print(ANDROID_LOG_ERROR, kLogTag, "libc symbols not found for redirect");
            return;
        }

        gum_interceptor_begin_transaction(interceptor);
        const GumReplaceReturn r1 = gum_interceptor_replace(
            interceptor, connect_addr, reinterpret_cast<gpointer>(hook_connect),
            reinterpret_cast<gpointer*>(&real_connect), nullptr);
        const GumReplaceReturn r2 = gum_interceptor_replace(
            interceptor, gai_addr, reinterpret_cast<gpointer>(hook_getaddrinfo),
            reinterpret_cast<gpointer*>(&real_getaddrinfo), nullptr);
        gum_interceptor_end_transaction(interceptor);
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "net redirect: connect=%d getaddrinfo=%d", r1, r2);
    }).detach();
}

} // namespace x2::android
