#include "Measure.h"

#include <limits>
#include <ratio>
#include <stdexcept>
#include <type_traits>

namespace qbook::bench {
std::uint64_t CheckedDurationNs(Clock::time_point begin, Clock::time_point end) {
  if (end < begin) throw std::invalid_argument("benchmark time points are reversed");
  using Rep = Clock::duration::rep;
  using Scale = std::ratio_divide<Clock::period,std::nano>;
  static_assert(std::is_integral_v<Rep> && sizeof(Rep)<=sizeof(std::uint64_t));
  // 先作无符号差值，避免跨越有符号 clock tick 的零点时发生 UB。
  const auto ticks=static_cast<std::uint64_t>(end.time_since_epoch().count())-
                   static_cast<std::uint64_t>(begin.time_since_epoch().count());
  const auto numerator=static_cast<std::uint64_t>(Scale::num);
  const auto denominator=static_cast<std::uint64_t>(Scale::den);
  return CheckedAdd(CheckedMultiply(ticks/denominator,numerator),
                    CheckedMultiply(ticks%denominator,numerator)/denominator);
}
void ValidateMeasurement(const PhaseMeasurement& value, bool valid) {
  if (value.phase.empty() || value.count_kind.empty())
    throw std::invalid_argument("measurement phase/count_kind is empty");
  if (value.count_kind == "none") {
    if (value.work_count != 0) throw std::invalid_argument("none requires work_count=0");
  } else if (valid && (value.work_count == 0 || value.duration_ns == 0)) {
    throw std::invalid_argument("rate measurement requires positive count and duration");
  }
}
}  // namespace qbook::bench
