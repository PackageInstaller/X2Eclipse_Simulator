#include "android/lebian_bypass.hpp"

#include <android/log.h>

#include "android/il2cpp.hpp"
#include "crypto/obf_string.hpp"

namespace x2::android {

namespace {

#define kLogTag (OBFLAZY("x2offline"))

using LeBianFn = void (*)(void* self, void* method);

LeBianFn g_on_lebian_finish = nullptr;

void start_lebian_replacement(void* self, void* method) {
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "StartLeBianUpdate bypassed -> OnLeBianFinish");
    g_on_lebian_finish(self, method);
}

} // namespace

void start_lebian_bypass() {
    il2cpp::on_ready([] {
        void* start = il2cpp::method("", OBFCSTR("AppMainImpl"), OBFCSTR("StartLeBianUpdate"), 0);
        g_on_lebian_finish = reinterpret_cast<LeBianFn>(
            il2cpp::method("", OBFCSTR("AppMainImpl"), OBFCSTR("OnLeBianFinish"), 0));
        void* original = nullptr;
        const bool ok = start && g_on_lebian_finish &&
                        il2cpp::replace(start, reinterpret_cast<void*>(start_lebian_replacement), &original);
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "LeBian bypass install: %d", ok);
    });
}

} // namespace x2::android
