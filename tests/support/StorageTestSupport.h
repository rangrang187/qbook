#pragma once

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace qbook::testing {

// 排他取得测试目录，只在析构时删除本实例创建的内容。
class TempDirectory {
public:
  TempDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() / "qbook-storage-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const auto* result = ::mkdtemp(buffer.data());
    if (!result) throw std::runtime_error("无法创建存储测试目录");
    path_ = result;
  }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;
  const std::filesystem::path& path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

}  // namespace qbook::testing
