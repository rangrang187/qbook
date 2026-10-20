#include <qlib/single_queue.h>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <type_traits>
#include <utility>

namespace {

struct message {
  int value;

  message() = delete;
  explicit message(int input) noexcept : value(input) {}
  message(const message&) = delete;
  message(message&& other) noexcept : value(std::exchange(other.value, -1)) {}
};

static_assert(!std::is_default_constructible_v<message>);
static_assert(std::is_nothrow_move_constructible_v<message>);
static_assert(std::is_nothrow_destructible_v<message>);

}  // namespace

bool check_single_queue() {
  qlib::single_queue<message> queue(1);
  std::atomic<std::uint64_t> atomic_probe{0};
  std::cout << "atomic_always_lock_free=" << std::atomic<std::uint64_t>::is_always_lock_free
            << " atomic_runtime_lock_free=" << atomic_probe.is_lock_free()
            << " cache_line=" << QLIB_CACHE_LINE
            << " queue_alignment=" << alignof(decltype(queue))
            << " queue_size=" << sizeof(queue) << '\n';

  if (!queue.try_emplace(7)) return false;
  message pending(9);
  const bool full_rejected = !queue.try_push(std::move(pending));
  const auto result = queue.try_pop();
  const bool value_matches = result.has_value() && result->value == 7;
  const auto empty_result = queue.try_pop();
  return queue.capacity() == 1 && full_rejected && pending.value == 9 && value_matches &&
      !empty_result.has_value() && queue.empty() && queue.size() == 0;
}
