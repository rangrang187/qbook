#pragma once

#include "BenchmarkCase.h"
#include <qlib/common.h>
#include <span>

namespace qbook::bench {
// 固定宽度小端标量，不读取记录的指针、padding 或 variant 对象表示。
class WorkloadHasher {
 public:
  void U64(std::uint64_t value);
  void I64(std::int64_t value) { U64(static_cast<std::uint64_t>(value)); }
  void String(std::string_view value);
  void Bytes(std::span<const std::byte> bytes);
  void Event(const qlib::feed_event& value);
  void Record(const qlib::record& value);
  std::string Finish() const;
 private:
  void Byte(std::uint8_t value);
  std::uint64_t state_ = 14695981039346656037ULL;
};
std::string HashWorkload(const CaseSpec& spec, std::uint64_t seed,
                         std::span<const qlib::feed_event> initial,
                         std::span<const qlib::feed_event> events);
std::string HashRecords(const CaseSpec& spec, std::uint64_t seed,
                        std::span<const qlib::record> records);
std::string HashFile(const std::filesystem::path& file);
}  // namespace qbook::bench
