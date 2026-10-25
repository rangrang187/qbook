#include "RunDirectory.h"

#include <stdexcept>
#include <string>
#include <system_error>

namespace qbook::detail {
std::filesystem::path ClaimRunDirectory(const std::filesystem::path& root,
                                       std::string_view seed, std::size_t max_attempts) {
  if (root.empty() || seed.empty() || seed.find_first_not_of(
      "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_") != std::string_view::npos ||
      max_attempts == 0)
    throw std::invalid_argument("运行目录需要非空根路径、安全候选标识和正尝试次数");
  std::filesystem::create_directories(root);
  for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
    const auto candidate = root / ("run-" + std::string(seed) + "-" + std::to_string(attempt));
    std::error_code error;
    const bool created = std::filesystem::create_directory(candidate, error);
    if (error) throw std::filesystem::filesystem_error("无法创建运行目录", candidate, error);
    if (created) return candidate;
  }
  throw std::runtime_error("运行目录候选已全部占用");
}
}  // namespace qbook::detail
