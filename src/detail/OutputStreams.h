#pragma once

#include <filesystem>
#include <functional>
#include <memory>

namespace arrow::io { class OutputStream; }
namespace qbook::detail {

using OutputStreamFactory =
    std::function<std::shared_ptr<arrow::io::OutputStream>(const std::filesystem::path&)>;

// 排他创建新文件；Arrow 成功接管 fd 后才解除局部所有权，失败抛异常。
std::shared_ptr<arrow::io::OutputStream> OpenExclusiveOutput(const std::filesystem::path& path);

}  // namespace qbook::detail
