#include "android/gm_button.hpp"

#include <cstdint>

#include <android/log.h>

#include "android/il2cpp.hpp"
#include "crypto/obf_string.hpp"

namespace x2::android {

namespace {

#define kLogTag (OBFLAZY("x2offline"))

using GetGameObjectFn = void* (*)(void* self, void* method);
using SetActiveFn = void (*)(void* go, bool value, void* method);
using OnOpenFn = void (*)(void* self, void* method);

using GetModuleFn = void* (*)(void* manager, void* type, void* method);

GetGameObjectFn il2cpp_get_game_object = nullptr;
SetActiveFn il2cpp_set_active = nullptr;
OnOpenFn real_login_on_open = nullptr;
std::ptrdiff_t cheat_buttons_offset = -1; // LoginPage.mButtonCheat (Button[])

OnOpenFn real_main_on_init = nullptr;
OnOpenFn real_button_press = nullptr;
std::ptrdiff_t main_gm_button_offset = -1; // MainPage.mBtnGM (Button)
void* main_gm_button = nullptr;
GetModuleFn module_manager_get_module = nullptr;
OnOpenFn gm_module_show = nullptr;
void* module_manager_class = nullptr;
void* gm_module_class = nullptr;

void set_button_active(void* button) {
    if (button == nullptr) return;
    if (void* go = il2cpp_get_game_object(button, nullptr)) il2cpp_set_active(go, true, nullptr);
}

void open_gm_tools() {
    void* manager = il2cpp::static_object(il2cpp::parent(module_manager_class), OBFCSTR("mInstance"));
    void* module = manager ? module_manager_get_module(manager, il2cpp::type_object(gm_module_class), nullptr) : nullptr;
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "GM click: manager=%p module=%p", manager, module);
    if (module != nullptr) gm_module_show(module, nullptr);
}


void main_on_init_hook(void* self, void* method) {
    real_main_on_init(self, method);
    if (self == nullptr) return;
    main_gm_button = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(self) + main_gm_button_offset);
    set_button_active(main_gm_button);
}

void button_press_hook(void* self, void* method) {
    if (self != nullptr && self == main_gm_button) {
        open_gm_tools();
        return;
    }
    real_button_press(self, method);
}


void unhide_cheat_buttons(void* login_page) {
    if (login_page == nullptr || cheat_buttons_offset < 0) return;
    void* array_ptr = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(login_page) +
                                                cheat_buttons_offset);
    if (array_ptr == nullptr) return;
    const std::int32_t length =
        *reinterpret_cast<std::int32_t*>(reinterpret_cast<std::uintptr_t>(array_ptr) + 24);
    void** elements = reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(array_ptr) + 32);
    int shown = 0;
    for (std::int32_t i = 0; i < length; ++i) {
        if (elements[i] == nullptr) continue;
        set_button_active(elements[i]);
        ++shown;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "cheat buttons shown: %d/%d", shown, length);
}

void login_on_open_hook(void* self, void* method) {
    real_login_on_open(self, method);
    unhide_cheat_buttons(self);
}

} // namespace

void install_gm_button_unhide() {
    il2cpp::on_ready([] {
        void* on_open = il2cpp::method("", OBFCSTR("LoginPage"), OBFCSTR("OnOpen"), 0);
        il2cpp_get_game_object = reinterpret_cast<GetGameObjectFn>(
            il2cpp::method(OBFCSTR("UnityEngine"), OBFCSTR("Component"), OBFCSTR("get_gameObject"), 0));
        il2cpp_set_active = reinterpret_cast<SetActiveFn>(
            il2cpp::method(OBFCSTR("UnityEngine"), OBFCSTR("GameObject"), OBFCSTR("SetActive"), 1));
        cheat_buttons_offset = il2cpp::field_offset("", OBFCSTR("LoginPage"), OBFCSTR("mButtonCheat"));
        const bool ok = on_open && il2cpp_get_game_object && il2cpp_set_active && cheat_buttons_offset >= 0 &&
                        il2cpp::replace(on_open, reinterpret_cast<void*>(login_on_open_hook),
                                        reinterpret_cast<void**>(&real_login_on_open));
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "gm button hook: %d (mButtonCheat=0x%tx)", ok,
                            cheat_buttons_offset);

        void* main_init = il2cpp::method("", OBFCSTR("MainPage"), OBFCSTR("OnInit"), 0);
        void* press = il2cpp::method(OBFCSTR("UnityEngine.UI"), OBFCSTR("Button"), OBFCSTR("Press"), 0);
        main_gm_button_offset = il2cpp::field_offset("", OBFCSTR("MainPage"), OBFCSTR("mBtnGM"));
        module_manager_class = il2cpp::klass(OBFCSTR("Foundation.Module"), OBFCSTR("ModuleManager"));
        gm_module_class = il2cpp::klass("", OBFCSTR("GMModule"));
        module_manager_get_module = reinterpret_cast<GetModuleFn>(
            il2cpp::method(OBFCSTR("Foundation.Module"), OBFCSTR("ModuleManager"), OBFCSTR("GetModule"), 1));
        gm_module_show = reinterpret_cast<OnOpenFn>(il2cpp::method("", OBFCSTR("GMModule"), OBFCSTR("Show"), 0));
        const bool main_ok = main_init && press && main_gm_button_offset >= 0 && module_manager_class &&
                             gm_module_class && module_manager_get_module && gm_module_show &&
                             il2cpp::replace(main_init, reinterpret_cast<void*>(main_on_init_hook),
                                             reinterpret_cast<void**>(&real_main_on_init)) &&
                             il2cpp::replace(press, reinterpret_cast<void*>(button_press_hook),
                                             reinterpret_cast<void**>(&real_button_press));
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "main GM entry hook: %d (mBtnGM=0x%tx)", main_ok,
                            main_gm_button_offset);
    });
}

} // namespace x2::android
