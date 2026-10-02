#include <jni.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <android/log.h>

#include "android/game_log.hpp"
#include "android/gm_button.hpp"
#include "android/il2cpp.hpp"
#include "android/lebian_bypass.hpp"
#include "android/log_hook.hpp"
#include "android/mail_refresh.hpp"
#include "android/net_redirect.hpp"
#include "core/log.hpp"
#include "crypto/obf_string.hpp"
#include "runtime/backend.hpp"

namespace {

#define kLogTag (OBFLAZY("x2offline"))

extern "C" const unsigned char _binary_tables_x2data_start[];
extern "C" const unsigned char _binary_tables_x2data_end[];

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


std::string files_dir() {
    const std::string pkg = package_name();
    if (pkg.empty()) return {};
    const std::string dir = "/data/data/" + pkg + "/files";
    mkdir(dir.c_str(), 0771);
    struct stat st{};
    return stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode) ? dir : std::string{};
}

bool host_backend() {
    char value[PROP_VALUE_MAX] = {};
    __system_property_get(OBFCSTR("debug.x2.backend"), value);
    return std::strcmp(value, OBFCSTR("host")) == 0;
}

bool is_main_process() {
    return package_name().find(':') == std::string::npos;
}

} // namespace

extern "C" __attribute__((constructor)) void x2_offline_entry() {
    if (!is_main_process()) {
        return;
    }

    const std::string files = files_dir();
    if (!files.empty()) {
        x2::core::log_set_file_dir(files);
    }

    const bool host = host_backend();
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "entry: pid=%d files=%s backend=%s", getpid(),
                        files.empty() ? "<none>" : files.c_str(), host ? "host" : "embedded");

    if (!host) {
        x2::runtime::start({_binary_tables_x2data_start,
                            static_cast<std::size_t>(_binary_tables_x2data_end - _binary_tables_x2data_start)});
        x2::core::log_line(x2::core::LogLevel::Info, "ENTRY", "embedded backend started");
    }

    x2::android::start_lebian_bypass();
    x2::android::install_gm_button_unhide();
    x2::android::install_game_log();
    x2::android::install_mail_refresh();
    x2::android::il2cpp::install();

    x2::android::install_net_redirect();
    x2::android::install_log_hook();
}

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "JNI_OnLoad vm=%p", vm);
    return JNI_VERSION_1_6;
}
