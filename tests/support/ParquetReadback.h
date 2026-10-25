#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include <qlib/common.h>

namespace arrow { class Table; }
namespace qbook::testing {

struct ReadbackResult {
  std::shared_ptr<arrow::Table> orders, trades, cancels, levels, books;
};

struct ExpectedRun {
  std::vector<qlib::order> orders;
  std::vector<qlib::trade> trades;
  std::vector<qlib::cancel> cancels;
  std::vector<qlib::level> levels;
  std::vector<qlib::book> books;
};

// 在写入线程结束后读取五文件；检查完整格式、非空值和行数上限，差异抛异常。
ReadbackResult ReadRunDirectory(const std::filesystem::path& directory,
                                std::int64_t max_rows_per_file);

// 按同类型行序比较所有业务字段；订单本机链接不属于文件格式。
void CheckExpectedFiles(const ReadbackResult& actual, const ExpectedRun& expected);

}  // namespace qbook::testing
