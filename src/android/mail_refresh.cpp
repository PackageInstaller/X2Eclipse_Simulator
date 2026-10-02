#include "android/mail_refresh.hpp"

#include <android/log.h>
#include <cstdint>
#include <cstring>

#include "android/il2cpp.hpp"
#include "crypto/obf_string.hpp"

namespace x2::android {

namespace {

#define kLogTag (OBFLAZY("x2offline"))

using CheckAllMailFn = void (*)(void* self, void* method);
using ShowFn = void (*)(void* self, void* method);
using OnMessageFn = void (*)(void* self, void* ws, void* message, void* method);
using StartFn = void (*)(void* self, void* method);
using GetModuleFn = void* (*)(void* manager, void* type, void* method);

CheckAllMailFn real_check_all_mail = nullptr;
ShowFn real_mail_module_show = nullptr;
OnMessageFn real_ws_on_message = nullptr;
StartFn real_ps_start = nullptr;

GetModuleFn module_manager_get_module = nullptr;
void* module_manager_class = nullptr;
void* mail_module_class = nullptr;
std::ptrdiff_t mail_check_offset = 0x70;

void* get_mail_module() {
    if (module_manager_class == nullptr || mail_module_class == nullptr || module_manager_get_module == nullptr) {
        return nullptr;
    }
    void* manager = il2cpp::static_object(il2cpp::parent(module_manager_class), OBFCSTR("mInstance"));
    if (manager == nullptr) return nullptr;
    return module_manager_get_module(manager, il2cpp::type_object(mail_module_class), nullptr);
}

void refresh_mail_module(void* mail_module) {
    if (mail_module == nullptr) return;
    if (mail_check_offset >= 0) {
        *reinterpret_cast<bool*>(reinterpret_cast<std::uintptr_t>(mail_module) + mail_check_offset) = true;
    }
    if (real_check_all_mail != nullptr) {
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "calling MailModule::CheckAllMail");
        real_check_all_mail(mail_module, nullptr);
    }
}

void trigger_mail_refresh() {
    void* mail_module = get_mail_module();
    if (mail_module != nullptr) {
        refresh_mail_module(mail_module);
    } else {
        __android_log_print(ANDROID_LOG_WARN, kLogTag, "MailModule not found for refresh");
    }
}

void mail_module_show_hook(void* self, void* method) {
    if (self != nullptr) {
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "MailModule::Show hooked -> ensuring fresh mail");
        refresh_mail_module(self);
    }
    real_mail_module_show(self, method);
}

void ws_on_message_hook(void* self, void* ws, void* message, void* method) {
    real_ws_on_message(self, ws, message, method);
    if (message == nullptr) return;
    const auto len = *reinterpret_cast<const std::int32_t*>(reinterpret_cast<std::uintptr_t>(message) + 0x10);
    const auto* chars = reinterpret_cast<const char16_t*>(reinterpret_cast<std::uintptr_t>(message) + 0x14);
    if (len <= 0 || chars == nullptr) return;
    static constexpr char16_t kPattern[] = u"MailEvent";
    constexpr std::int32_t kPatternLen = static_cast<std::int32_t>(sizeof(kPattern) / sizeof(char16_t) - 1);
    bool is_mail = false;
    if (len >= kPatternLen) {
        for (std::int32_t i = 0; i <= len - kPatternLen; ++i) {
            if (std::memcmp(chars + i, kPattern, kPatternLen * sizeof(char16_t)) == 0) {
                is_mail = true;
                break;
            }
        }
    }

    if (is_mail) {
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "WS message contains MailEvent -> triggering real-time refresh!");
        trigger_mail_refresh();
    }
}

void ps_start_hook(void* self, void* method) {
    real_ps_start(self, method);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "PublicServiceManager::Start hooked -> initial mail sync");
    trigger_mail_refresh();
}

} // namespace

void install_mail_refresh() {
    il2cpp::on_ready([] {
        module_manager_class = il2cpp::klass(OBFCSTR("Foundation.Module"), OBFCSTR("ModuleManager"));
        module_manager_get_module = reinterpret_cast<GetModuleFn>(
            il2cpp::method(OBFCSTR("Foundation.Module"), OBFCSTR("ModuleManager"), OBFCSTR("GetModule"), 1));
        mail_module_class = il2cpp::klass("", OBFCSTR("MailModule"));

        const auto offset = il2cpp::field_offset("", OBFCSTR("MailModule"), OBFCSTR("check"));
        if (offset >= 0) {
            mail_check_offset = offset;
        }

        void* check_all = il2cpp::method("", OBFCSTR("MailModule"), OBFCSTR("CheckAllMail"), 0);
        if (check_all != nullptr) {
            real_check_all_mail = reinterpret_cast<CheckAllMailFn>(check_all);
        }

        void* show = il2cpp::method("", OBFCSTR("MailModule"), OBFCSTR("Show"), 0);
        if (show != nullptr) {
            il2cpp::replace(show, reinterpret_cast<void*>(mail_module_show_hook),
                            reinterpret_cast<void**>(&real_mail_module_show));
        }

        void* on_msg = il2cpp::method(OBFCSTR("apps_core"), OBFCSTR("WSCometServiceWrapper"), OBFCSTR("OnMessage"), 2);
        if (on_msg != nullptr) {
            il2cpp::replace(on_msg, reinterpret_cast<void*>(ws_on_message_hook),
                            reinterpret_cast<void**>(&real_ws_on_message));
        }

        void* ps_start = il2cpp::method("", OBFCSTR("PublicServiceManager"), OBFCSTR("Start"), 0);
        if (ps_start != nullptr) {
            il2cpp::replace(ps_start, reinterpret_cast<void*>(ps_start_hook),
                            reinterpret_cast<void**>(&real_ps_start));
        }

        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "mail refresh hooks installed: show=%d on_msg=%d ps_start=%d check_all=%d check_off=0x%zx",
                            show != nullptr, on_msg != nullptr, ps_start != nullptr, check_all != nullptr,
                            static_cast<std::size_t>(mail_check_offset));
    });
}

} // namespace x2::android
