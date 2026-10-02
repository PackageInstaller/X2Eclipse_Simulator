#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace x2::runtime {


void start(std::span<const std::uint8_t> tables, std::uint16_t http_port = 9999,
           std::uint16_t tcp_port = 10001);

void start_from_file(std::string_view tables_path, std::uint16_t http_port = 9999,
                     std::uint16_t tcp_port = 10001);

std::vector<std::uint8_t> read_file(std::string_view path);

} // namespace x2::runtime
