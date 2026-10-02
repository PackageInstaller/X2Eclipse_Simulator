cmake_minimum_required(VERSION 3.21)
project(x2offline C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(KEEP_SYMBOLS OFF)
set(ANDROID_ELF_PAGE_SIZE "65536" CACHE STRING "Android ELF PT_LOAD 最大页大小(4096/16384/65536)")
set_property(CACHE ANDROID_ELF_PAGE_SIZE PROPERTY STRINGS 4096 16384 65536)
get_filename_component(X2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)

find_package(Threads REQUIRED)

set(TABLES_BLOB "${X2_ROOT}/data/tables.x2data")
set(TABLES_OBJ "${CMAKE_CURRENT_BINARY_DIR}/tables_x2.o")
add_custom_command(
  OUTPUT ${TABLES_OBJ}
  COMMAND ${CMAKE_COMMAND} -E copy ${TABLES_BLOB} tables.x2data
  COMMAND ${CMAKE_OBJCOPY}
    -I binary -O elf64-littleaarch64 -B aarch64 tables.x2data tables_x2.o
  DEPENDS ${TABLES_BLOB}
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  COMMENT "Embedding tables blob: ${TABLES_BLOB}"
  VERBATIM)

add_library(x2offline SHARED
  ${X2_ROOT}/src/core/log.cpp
  ${X2_ROOT}/src/proto/protobuf.cpp
  ${X2_ROOT}/src/server/http.cpp
  ${X2_ROOT}/src/server/websocket.cpp
  ${X2_ROOT}/src/server/marsnet.cpp
  ${X2_ROOT}/src/server/router.cpp
  ${X2_ROOT}/src/server/tcp_server.cpp
  ${X2_ROOT}/src/offline/table_blob.cpp
  ${X2_ROOT}/src/offline/game_table.cpp
  ${X2_ROOT}/src/offline/account_repository.cpp
  ${X2_ROOT}/src/offline/protocol_builders.cpp
  ${X2_ROOT}/src/offline/cheat.cpp
  ${X2_ROOT}/src/offline/mission.cpp
  ${X2_ROOT}/src/offline/shop.cpp
  ${X2_ROOT}/src/offline/achievement.cpp
  ${X2_ROOT}/src/offline/npc_social.cpp
  ${X2_ROOT}/src/offline/medal.cpp
  ${X2_ROOT}/src/runtime/backend.cpp
  ${X2_ROOT}/src/persist/account_db.cpp
  ${X2_ROOT}/src/third_party/database/sqlite/sqlite3.c
  ${X2_ROOT}/src/android/il2cpp.cpp
  ${X2_ROOT}/src/android/lebian_bypass.cpp
  ${X2_ROOT}/src/android/net_redirect.cpp
  ${X2_ROOT}/src/android/log_hook.cpp
  ${X2_ROOT}/src/android/gm_button.cpp
  ${X2_ROOT}/src/android/mail_refresh.cpp
  ${X2_ROOT}/src/android/game_log.cpp
  ${X2_ROOT}/src/android/entry.cpp
  ${TABLES_OBJ})

set_source_files_properties(${X2_ROOT}/src/third_party/database/sqlite/sqlite3.c PROPERTIES
  COMPILE_OPTIONS "-w"
  COMPILE_DEFINITIONS "SQLITE_THREADSAFE=2;SQLITE_OMIT_LOAD_EXTENSION;SQLITE_DEFAULT_MEMSTATUS=0;SQLITE_OMIT_DEPRECATED")

target_include_directories(x2offline PRIVATE
  ${X2_ROOT}/src
  ${X2_ROOT}/src/third_party/database/sqlite
  ${X2_ROOT}/src/third_party/database/sqlite_orm
  ${X2_ROOT}/src/third_party/frida-gum)

target_link_libraries(x2offline PRIVATE
  log dl m z
  ${X2_ROOT}/src/third_party/frida-gum/libfrida-gum.a)

target_compile_options(x2offline PRIVATE -Wno-format-security)
if(KEEP_SYMBOLS)
  target_compile_options(x2offline PRIVATE -O2 -g)
  target_compile_definitions(x2offline PRIVATE X2_KEEP_SYMBOLS=1)
  target_link_options(x2offline PRIVATE -static-libstdc++)
else()
  target_compile_options(x2offline PRIVATE -O2 -fvisibility=hidden)
  target_link_options(x2offline PRIVATE
    -static-libstdc++
    -Wl,--exclude-libs,ALL
    -s)
endif()
target_link_options(x2offline PRIVATE
  "-Wl,-z,max-page-size=${ANDROID_ELF_PAGE_SIZE}")

set(X2_SO_OUT_DIR "${CMAKE_BINARY_DIR}/lib/arm64-v8a" CACHE PATH
  "libx2offline.so 输出目录(默认 build/lib/arm64-v8a)")
set_target_properties(x2offline PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${X2_SO_OUT_DIR}")

message(STATUS "Target: libx2offline.so (embedded tables: ${TABLES_BLOB})")
