#pragma once

#include <cstddef>
#include <filesystem>
#include <string_view>

namespace qbook::detail {
// 排他取得新的运行目录；已有目录换候选名，其余文件系统错误向外传播。
std::filesystem::path ClaimRunDirectory(const std::filesystem::path& root,
                                       std::string_view seed, std::size_t max_attempts);
}  // namespace qbook::detail
