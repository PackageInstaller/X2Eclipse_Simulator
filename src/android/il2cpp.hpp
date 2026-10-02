#pragma once

#include <cstddef>
#include <functional>

namespace x2::android::il2cpp {


void install();
void on_ready(std::function<void()> callback);
void* method(const char* name_space, const char* klass, const char* name, int argc);
std::ptrdiff_t field_offset(const char* name_space, const char* klass, const char* field);
void* klass(const char* name_space, const char* name);
void* parent(void* klass);
void* type_object(void* klass);
void* static_object(void* klass, const char* field);
bool replace(void* target, void* replacement, void** original);

} // namespace x2::android::il2cpp
