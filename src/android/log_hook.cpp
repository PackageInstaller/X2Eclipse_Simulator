#include "android/log_hook.hpp"

#include <android/log.h>
#include <cstdarg>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <thread>

#define GUM_STATIC
#include "frida-gum.h"

#include "core/log.hpp"
#include "crypto/obf_string.hpp"

namespace x2::android {

namespace {

#define kLogTag (OBFLAZY("x2offline"))

using LogWriteFn = int (*)(int prio, const char* tag, const char* text);
using LogPrintFn = int (*)(int prio, const char* tag, const char* fmt, ...);
using LogBufWriteFn = int (*)(int buf_id, int prio, const char* tag, const char* text);
using LogBufPrintFn = int (*)(int buf_id, int prio, const char* tag, const char* fmt, ...);
using LogVPrintFn = int (*)(int prio, const char* tag, const char* fmt, va_list ap);
using LogBufVPrintFn = int (*)(int buf_id, int prio, const char* tag, const char* fmt, va_list ap);

LogWriteFn real_log_write = nullptr;
LogPrintFn real_log_print = nullptr;
LogBufWriteFn real_log_buf_write = nullptr;
LogBufPrintFn real_log_buf_print = nullptr;
LogVPrintFn real_log_vprint = nullptr;
LogBufVPrintFn real_log_buf_vprint = nullptr;

void capture(int prio, const char* tag, const char* text) {
    if (text == nullptr) return;
    if (tag != nullptr && std::strcmp(tag, kLogTag) == 0) return;
    x2::core::log_capture(std::string(tag == nullptr ? "?" : tag), text);
    (void)prio;
}

int hook_log_write(int prio, const char* tag, const char* text) {
    capture(prio, tag, text);
    return real_log_write(prio, tag, text);
}

int hook_log_buf_write(int buf_id, int prio, const char* tag, const char* text) {
    capture(prio, tag, text);
    return real_log_buf_write(buf_id, prio, tag, text);
}

int hook_log_print(int prio, const char* tag, const char* fmt, ...) {
    va_list ap{};
    va_start(ap, fmt);
    char buffer[4096];
    const int written = vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    capture(prio, tag, buffer);
    va_start(ap, fmt);
    const int result = real_log_vprint(prio, tag, fmt, ap);
    va_end(ap);
    (void)written;
    return result;
}

int hook_log_buf_print(int buf_id, int prio, const char* tag, const char* fmt, ...) {
    va_list ap{};
    va_start(ap, fmt);
    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    capture(prio, tag, buffer);
    va_start(ap, fmt);
    const int result = real_log_buf_vprint(buf_id, prio, tag, fmt, ap);
    va_end(ap);
    return result;
}

void* liblog_symbol(const char* name) {
    void* handle = dlopen("liblog.so", RTLD_NOW | RTLD_NOLOAD);
    if (!handle) handle = dlopen("liblog.so", RTLD_NOW);
    if (!handle) return nullptr;
    void* addr = dlsym(handle, name);
    dlclose(handle);
    return addr;
}

int replace(GumInterceptor* interceptor, void* target, void* replacement, void** original) {
    return gum_interceptor_replace(interceptor, target, replacement, original, nullptr);
}

} // namespace

void install_log_hook() {
    std::thread([] {
        gum_init_embedded();

        auto* interceptor = gum_interceptor_obtain();
        real_log_vprint = reinterpret_cast<LogVPrintFn>(liblog_symbol("__android_log_vprint"));
        real_log_buf_vprint = reinterpret_cast<LogBufVPrintFn>(liblog_symbol("__android_log_buf_vprint"));

        gum_interceptor_begin_transaction(interceptor);
        int installed = 0;
        if (void* addr = liblog_symbol("__android_log_write")) {
            installed += replace(interceptor, addr, reinterpret_cast<void*>(hook_log_write),
                                 reinterpret_cast<void**>(&real_log_write)) == GUM_REPLACE_OK;
        }
        if (void* addr = liblog_symbol("__android_log_print")) {
            installed += replace(interceptor, addr, reinterpret_cast<void*>(hook_log_print),
                                 reinterpret_cast<void**>(&real_log_print)) == GUM_REPLACE_OK;
        }
        if (void* addr = liblog_symbol("__android_log_buf_write")) {
            installed += replace(interceptor, addr, reinterpret_cast<void*>(hook_log_buf_write),
                                 reinterpret_cast<void**>(&real_log_buf_write)) == GUM_REPLACE_OK;
        }
        if (void* addr = liblog_symbol("__android_log_buf_print")) {
            installed += replace(interceptor, addr, reinterpret_cast<void*>(hook_log_buf_print),
                                 reinterpret_cast<void**>(&real_log_buf_print)) == GUM_REPLACE_OK;
        }
        gum_interceptor_end_transaction(interceptor);
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "log hook installed: %d", installed);
    }).detach();
}

} // namespace x2::android
