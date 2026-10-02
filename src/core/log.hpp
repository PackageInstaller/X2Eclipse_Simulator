#pragma once

#include <source_location>
#include <string>
#include <string_view>

namespace x2::core {

enum class LogLevel { Debug, Info, Warn, Error };

void log_line(LogLevel level, std::string_view tag, std::string_view message,
              std::source_location where = std::source_location::current());
void log_error_errno(std::string_view tag, std::string_view operation,
                     std::source_location where = std::source_location::current());


void log_set_file_dir(std::string_view base);
[[nodiscard]] std::string_view log_file_path();


void log_capture(std::string_view source_tag, std::string_view message);

} // namespace x2::core
