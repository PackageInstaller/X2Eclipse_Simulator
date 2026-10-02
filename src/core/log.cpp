#include "core/log.hpp"

#include <cerrno>
#include <cstring>
#include <format>
#include <string>
#include <string_view>

#ifdef __ANDROID__
#include <android/log.h>
#else
#include <cstdio>
#include <print>
#endif

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace x2::core {

namespace {

constexpr std::string_view level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}


constexpr std::string_view level_color(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "\033[90m";
        case LogLevel::Info: return "\033[32m";
        case LogLevel::Warn: return "\033[33m";
        case LogLevel::Error: return "\033[31m";
    }
    return "\033[0m";
}

std::string timestamp_now() {
    struct timespec ts {};
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm {};
    localtime_r(&ts.tv_sec, &tm);
    char buf[16] = {};
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

constexpr int kMaxLogSessions = 5;

std::mutex g_sink_mutex;
std::string g_log_dir;          // <files>/logs/<session>, empty = disabled
std::string g_log_file_path;
int g_log_fd = -1;

#ifdef __ANDROID__
android_LogPriority android_level(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return ANDROID_LOG_DEBUG;
        case LogLevel::Info: return ANDROID_LOG_INFO;
        case LogLevel::Warn: return ANDROID_LOG_WARN;
        case LogLevel::Error: return ANDROID_LOG_ERROR;
    }
    return ANDROID_LOG_INFO;
}
#endif

void prune_log_sessions(const std::string& parent) {
    std::vector<std::string> sessions;
    if (DIR* d = opendir(parent.c_str())) {
        while (struct dirent* ent = readdir(d)) {
            const std::string child = parent + "/" + ent->d_name;
            struct stat st {};
            if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) continue;
            if (stat(child.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            sessions.push_back(child);
        }
        closedir(d);
    }
    if (sessions.size() <= static_cast<std::size_t>(kMaxLogSessions)) return;
    std::sort(sessions.begin(), sessions.end());
    for (std::size_t i = 0; i + kMaxLogSessions < sessions.size(); ++i) {
        if (DIR* d = opendir(sessions[i].c_str())) {
            while (struct dirent* ent = readdir(d)) {
                if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) continue;
                unlink((sessions[i] + "/" + ent->d_name).c_str());
            }
            closedir(d);
        }
        rmdir(sessions[i].c_str());
    }
}

void write_line(std::string_view line) {
    std::lock_guard lock{g_sink_mutex};
    if (g_log_fd < 0) return;
    const ssize_t ignored = ::write(g_log_fd, line.data(), line.size());
    (void)ignored;
}

} // namespace


void log_set_file_dir(std::string_view base) {
    std::lock_guard lock{g_sink_mutex};
    if (g_log_fd >= 0) return;
    const std::string parent = std::string{base} + "/logs";

    char session[64] = {};
    struct timespec ts {};
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm {};
    localtime_r(&ts.tv_sec, &tm);
    snprintf(session, sizeof(session), "%04d%02d%02d_%02d%02d%02d_%d", tm.tm_year + 1900,
             tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(getpid()));

    const std::string dir = parent + "/" + session;
    mkdir(parent.c_str(), 0755);
    mkdir(dir.c_str(), 0755);
    prune_log_sessions(parent);

    g_log_dir = dir;
    g_log_file_path = dir + "/x2.log";
    g_log_fd = open(g_log_file_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
}

std::string_view log_file_path() {
    std::lock_guard lock{g_sink_mutex};
    return g_log_file_path;
}

void log_capture(std::string_view source_tag, std::string_view message) {
    const std::string line = std::format("[{}] {}\n", source_tag, message);
    write_line(line);
}

void log_line(LogLevel level, std::string_view tag, std::string_view message,
              std::source_location where) {
    auto file = std::string_view{where.file_name()};
    if (const auto pos = file.find_last_of('/'); pos != std::string_view::npos) {
        file = file.substr(pos + 1);
    }
    const auto stamp = timestamp_now();
    const std::string line = std::format("[{}] [{}] [{}] [{}:{}] {}\n", stamp, level_name(level), tag,
                                         file, where.line(), message);
#ifdef __ANDROID__
    __android_log_print(android_level(level), "x2offline", "%.*s",
                        static_cast<int>(line.size() - 1), line.data());
#else
    const bool tty = isatty(fileno(stdout)) == 1;
    const std::string colored =
        tty ? std::format("\033[90m{}\033[0m [{}{}\033[0m] [{}] [{}:{}] {}\n", stamp,
                          level_color(level), level_name(level), tag, file, where.line(), message)
            : line;
    std::print("{}", colored);
    std::fflush(stdout);
#endif
    write_line(line);
}

void log_error_errno(std::string_view tag, std::string_view operation, std::source_location where) {
    log_line(LogLevel::Error, tag, std::format("{} failed: errno={} ({})", operation, errno, std::strerror(errno)), where);
}

} // namespace x2::core
