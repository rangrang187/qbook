#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

#include <qlib/common.h>

namespace arrow {
class Schema;
namespace io { class OutputStream; }
}
namespace qbook {
class WriterThread;
namespace testing { struct ParquetWriterTestAccess; }

// 由单一线程按类型保存原始字段；显式 Close 成功后才完成五文件保存。
class ParquetWriter {
public:
  // run_dir 由调用者独占取得；批量须为可表示的正数，同名文件导致失败。
  ParquetWriter(const std::filesystem::path& run_dir, std::int64_t batch_rows);
  ~ParquetWriter() noexcept;
  ParquetWriter(const ParquetWriter&) = delete;
  ParquetWriter& operator=(const ParquetWriter&) = delete;

  // 复制一条完整记录并按类型触发满批写入；工作失败后禁止继续追加。
  void Add(const qlib::order& value);
  void Add(const qlib::trade& value);
  void Add(const qlib::cancel& value);
  void Add(const qlib::level& value);
  void Add(const qlib::book& value);

  // 保存尾批并关闭全部资源；成功后幂等，失败保留首错且禁止重放。
  void Close();

private:
  using Factory = std::function<std::shared_ptr<arrow::io::OutputStream>(const std::filesystem::path&)>;
  ParquetWriter(const std::filesystem::path& run_dir, std::int64_t batch_rows,
                const Factory& output_factory);
  enum class State { Open, Closed, Failed };
  struct Sink;
  static std::unique_ptr<Sink> OpenSink(const std::filesystem::path&, std::int64_t,
                                        std::shared_ptr<arrow::Schema>, const Factory&);
  static std::unique_ptr<Sink> OpenOrders(const std::filesystem::path&, std::int64_t, const Factory&);
  static std::unique_ptr<Sink> OpenTrades(const std::filesystem::path&, std::int64_t, const Factory&);
  static std::unique_ptr<Sink> OpenCancels(const std::filesystem::path&, std::int64_t, const Factory&);
  static std::unique_ptr<Sink> OpenLevels(const std::filesystem::path&, std::int64_t, const Factory&);
  static std::unique_ptr<Sink> OpenBooks(const std::filesystem::path&, std::int64_t, const Factory&);
  static void AppendOrder(Sink&, const qlib::order&);
  static void AppendTrade(Sink&, const qlib::trade&);
  static void AppendCancel(Sink&, const qlib::cancel&);
  static void AppendLevel(Sink&, const qlib::level&);
  static void AppendBook(Sink&, const qlib::book&);
  static void Reserve(Sink&);
  static void Commit(Sink&);
  static void Flush(Sink&);
  void RequireOpen() const;
  void Cleanup() noexcept;

  std::unique_ptr<Sink> orders_, trades_, cancels_, levels_, books_;
  State state_ = State::Open;
  friend class WriterThread;
  friend struct testing::ParquetWriterTestAccess;
};

}  // namespace qbook
