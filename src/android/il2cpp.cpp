#include "android/il2cpp.hpp"

#include <android/dlext.h>
#include <android/log.h>
#include <atomic>
#include <cstring>
#include <dlfcn.h>
#include <vector>

#define GUM_STATIC
#include "frida-gum.h"

#include "crypto/obf_string.hpp"

namespace x2::android::il2cpp {

namespace {

#define kLogTag (OBFLAZY("x2offline"))

using LoaderDlopenFn = void* (*)(const char*, int, const void*);
using LoaderDlopenExtFn = void* (*)(const char*, int, const android_dlextinfo*, const void*);
using InitFn = void (*)(const char*);

struct Api {
    void* (*domain_get)();
    void** (*domain_get_assemblies)(void*, std::size_t*);
    void* (*assembly_get_image)(void*);
    void* (*class_from_name)(void*, const char*, const char*);
    void* (*class_get_method_from_name)(void*, const char*, int);
    void* (*class_get_field_from_name)(void*, const char*);
    std::size_t (*field_get_offset)(void*);
    void* (*class_get_parent)(void*);
    void* (*class_get_type)(void*);
    void* (*type_get_object)(void*);
    void (*field_static_get_value)(void*, void*);
} api{};

LoaderDlopenFn real_dlopen = nullptr;
LoaderDlopenExtFn real_dlopen_ext = nullptr;
InitFn real_init = nullptr;
std::atomic<bool> armed{false};

std::vector<std::function<void()>>& callbacks() {
    static std::vector<std::function<void()>> list;
    return list;
}

void init_hook(const char* domain) {
    real_init(domain);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "il2cpp ready, running %zu hook installers",
                        callbacks().size());
    for (auto& callback : callbacks()) callback();
}

template <class Fn>
void bind(void* handle, Fn& fn, const char* name) {
    fn = reinterpret_cast<Fn>(dlsym(handle, name));
}

void arm(void* handle, const char* path) {
    if (handle == nullptr || path == nullptr || std::strstr(path, OBFCSTR("libil2cpp.so")) == nullptr) return;
    if (armed.exchange(true)) return;
    bind(handle, api.domain_get, OBFCSTR("il2cpp_domain_get"));
    bind(handle, api.domain_get_assemblies, OBFCSTR("il2cpp_domain_get_assemblies"));
    bind(handle, api.assembly_get_image, OBFCSTR("il2cpp_assembly_get_image"));
    bind(handle, api.class_from_name, OBFCSTR("il2cpp_class_from_name"));
    bind(handle, api.class_get_method_from_name, OBFCSTR("il2cpp_class_get_method_from_name"));
    bind(handle, api.class_get_field_from_name, OBFCSTR("il2cpp_class_get_field_from_name"));
    bind(handle, api.field_get_offset, OBFCSTR("il2cpp_field_get_offset"));
    bind(handle, api.class_get_parent, OBFCSTR("il2cpp_class_get_parent"));
    bind(handle, api.class_get_type, OBFCSTR("il2cpp_class_get_type"));
    bind(handle, api.type_get_object, OBFCSTR("il2cpp_type_get_object"));
    bind(handle, api.field_static_get_value, OBFCSTR("il2cpp_field_static_get_value"));
    void* init = dlsym(handle, OBFCSTR("il2cpp_init"));
    const bool ok = init != nullptr && replace(init, reinterpret_cast<void*>(init_hook),
                                               reinterpret_cast<void**>(&real_init));
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "libil2cpp loaded, il2cpp_init hook=%d", ok);
}

void* dlopen_hook(const char* path, int flags, const void* caller) {
    void* handle = real_dlopen(path, flags, caller);
    arm(handle, path);
    return handle;
}

void* dlopen_ext_hook(const char* path, int flags, const android_dlextinfo* info, const void* caller) {
    void* handle = real_dlopen_ext(path, flags, info, caller);
    arm(handle, path);
    return handle;
}

void* find_class(const char* name_space, const char* klass) {
    if (api.domain_get == nullptr) return nullptr;
    std::size_t count = 0;
    void** assemblies = api.domain_get_assemblies(api.domain_get(), &count);
    for (std::size_t i = 0; i < count; ++i) {
        if (void* found = api.class_from_name(api.assembly_get_image(assemblies[i]), name_space, klass)) {
            return found;
        }
    }
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "class not found: %s.%s", name_space, klass);
    return nullptr;
}

} // namespace

void on_ready(std::function<void()> callback) {
    callbacks().push_back(std::move(callback));
}

void install() {
    gum_init_embedded();
    GumModule* linker = gum_process_find_module_by_name(OBFCSTR("linker64"));
    const auto dlopen_addr = linker ? gum_module_find_export_by_name(linker, OBFCSTR("__loader_dlopen")) : 0;
    const auto ext_addr = linker ? gum_module_find_export_by_name(linker, OBFCSTR("__loader_android_dlopen_ext")) : 0;
    const bool ok = dlopen_addr != 0 && ext_addr != 0 &&
                    replace(reinterpret_cast<void*>(dlopen_addr), reinterpret_cast<void*>(dlopen_hook),
                            reinterpret_cast<void**>(&real_dlopen)) &&
                    replace(reinterpret_cast<void*>(ext_addr), reinterpret_cast<void*>(dlopen_ext_hook),
                            reinterpret_cast<void**>(&real_dlopen_ext));
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "linker hook=%d", ok);
}

void* method(const char* name_space, const char* klass, const char* name, int argc) {
    void* cls = find_class(name_space, klass);
    void* info = cls ? api.class_get_method_from_name(cls, name, argc) : nullptr;
    if (info == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "method not found: %s::%s/%d", klass, name, argc);
        return nullptr;
    }
    return *static_cast<void**>(info); // MethodInfo::methodPointer is the first member.
}

std::ptrdiff_t field_offset(const char* name_space, const char* klass, const char* field) {
    void* cls = find_class(name_space, klass);
    void* info = cls ? api.class_get_field_from_name(cls, field) : nullptr;
    return info ? static_cast<std::ptrdiff_t>(api.field_get_offset(info)) : -1;
}

void* klass(const char* name_space, const char* name) {
    return find_class(name_space, name);
}

void* parent(void* cls) {
    return cls ? api.class_get_parent(cls) : nullptr;
}

void* type_object(void* cls) {
    return cls ? api.type_get_object(api.class_get_type(cls)) : nullptr;
}

void* static_object(void* cls, const char* field) {
    void* info = cls ? api.class_get_field_from_name(cls, field) : nullptr;
    void* value = nullptr;
    if (info) api.field_static_get_value(info, &value);
    return value;
}

bool replace(void* target, void* replacement, void** original) {
    auto* interceptor = gum_interceptor_obtain();
    gum_interceptor_begin_transaction(interceptor);
    const auto status = gum_interceptor_replace_fast(interceptor, target, replacement, original, nullptr);
    gum_interceptor_end_transaction(interceptor);
    return status == GUM_REPLACE_OK;
}

} // namespace x2::android::il2cpp
