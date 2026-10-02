#include "android/game_log.hpp"

#include <cstring>
#include <sys/system_properties.h>

#include <android/log.h>

#include "android/il2cpp.hpp"
#include "crypto/obf_string.hpp"

namespace x2::android {

namespace {

#define kLogTag (OBFLAZY("x2offline"))

using ConfigSuccessFn = void (*)(void* self, void* data, void* method);
using SetLogStatusFn = void (*)(bool enable, void* method);

ConfigSuccessFn real_config_success = nullptr;
SetLogStatusFn set_log_status = nullptr;


void config_success_hook(void* self, void* data, void* method) {
    real_config_success(self, data, method);
    set_log_status(true, nullptr);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "game log enabled");
}

} // namespace

void install_game_log() {
    char value[PROP_VALUE_MAX] = {};
    __system_property_get(OBFCSTR("debug.x2.gamelog"), value);
    if (std::strcmp(value, "0") == 0) return;
    il2cpp::on_ready([] {
        void* target = il2cpp::method("", OBFCSTR("AppConfig"), OBFCSTR("LoadGameConfigSuccess"), 1);
        set_log_status = reinterpret_cast<SetLogStatusFn>(
            il2cpp::method("", OBFCSTR("LogicHelper"), OBFCSTR("SetLogStatus"), 1));
        const bool ok = target && set_log_status &&
                        il2cpp::replace(target, reinterpret_cast<void*>(config_success_hook),
                                        reinterpret_cast<void**>(&real_config_success));
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "game log hook: %d", ok);
    });
}

} // namespace x2::android
