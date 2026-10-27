#pragma once

#include "detail/MetricsLayout.h"

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace qbook::testing {

inline constexpr std::array<std::string_view, 6> metrics_fixture_names{
    "layout", "bounds-min", "bounds-max", "large-a", "large-b", "zero-samples"};

// 独立 JSON 期望不由本函数生成；此处只构造真实 qlib 普通值的固定样本。
inline qlib::metrics MetricsFixture(std::string_view name) {
  if (name == "layout")
    return {9'007'199'254'741'111LL, 101, 202, 303, 404, 505, 606,
            9'007'199'254'740'993ULL, 808, 909, 1010, 1111, 1212, 1313, 1414, 1515, 1616};
  if (name == "bounds-min") {
    qlib::metrics sample{};
    sample.ts_ns = std::numeric_limits<std::int64_t>::min();
    return sample;
  }
  if (name == "bounds-max") {
    constexpr auto u = std::numeric_limits<std::uint64_t>::max();
    return {std::numeric_limits<std::int64_t>::max(), u, u, u, u, u, u, u, u, u,
            u, u, u, u, u, u, u};
  }
  if (name == "large-a" || name == "large-b") {
    const bool next = name == "large-b";
    return {next ? 9'007'199'454'740'993LL : 9'007'199'254'740'993LL,
            100, 1000, 80, 20, 0, 0,
            next ? 9'007'199'254'741'003ULL : 9'007'199'254'740'993ULL,
            next ? 1200ULL : 1000ULL, next ? 12ULL : 10ULL,
            2, 3, 4096, 80, 20, 0, 4};
  }
  if (name == "zero-samples")
    return {123, 10, 100, 9, 1, 0, 0, 10, 0, 0, 1, 2, 2048, 9, 1, 0, 2};
  throw std::invalid_argument("unknown fixed metrics fixture");
}

}  // namespace qbook::testing
