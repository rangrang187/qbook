#pragma once

#include "BenchmarkCase.h"

#include <chrono>
#include <utility>

namespace qbook::bench {
using Clock = std::chrono::steady_clock;
struct BatchObservation { std::uint64_t work_count = 0; std::uint64_t checksum = 0; };

std::uint64_t CheckedDurationNs(Clock::time_point begin, Clock::time_point end);
void ValidateMeasurement(const PhaseMeasurement& value, bool valid);

// 仅约束编译器重排；不是 CPU fence，也不清空缓存。
inline void CompilerBoundary() noexcept {
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" ::: "memory");
#else
#error "V5 benchmark needs a verified compiler boundary adapter"
#endif
}
inline void Consume(std::uint64_t value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" : : "r"(value) : "memory");
#endif
}

template <class Kernel>
PhaseMeasurement MeasureBatch(std::string phase, std::string count_kind, Kernel&& kernel) {
  CompilerBoundary();
  const auto begin = Clock::now();
  const BatchObservation observation = std::forward<Kernel>(kernel)();
  Consume(observation.work_count);
  Consume(observation.checksum);
  const auto end = Clock::now();
  CompilerBoundary();
  return {std::move(phase), std::move(count_kind), observation.work_count,
          CheckedDurationNs(begin, end), observation.checksum};
}
}  // namespace qbook::bench
