#include <array>
#include <cstddef>
#include <exception>
#include <iostream>

static_assert(__cplusplus > 202002L);

bool check_common();
bool check_hive();
bool check_map();
bool check_single_queue();
bool check_pool();
bool check_memory_pool();

int main() {
  const std::array<const char*, 6> names{
    "check_common", "check_hive", "check_map",
    "check_single_queue", "check_pool", "check_memory_pool"
  };
  const std::array<bool (*)(), 6> checks{
    check_common, check_hive, check_map,
    check_single_queue, check_pool, check_memory_pool
  };
  std::array<bool, 6> results{};

  // 每项独立捕获异常，全部调用结束后再汇总结果。
  for (std::size_t index = 0; index < checks.size(); ++index) {
    try {
      results[index] = checks[index]();
    } catch (const std::exception& error) {
      std::cerr << names[index] << " 异常：" << error.what() << '\n';
    } catch (...) {
      std::cerr << names[index] << " 遇到未知异常\n";
    }
    std::cout << names[index] << ": " << (results[index] ? "PASS" : "FAIL") << '\n';
  }

  std::size_t passed = 0;
  for (bool result : results) {
    if (result) ++passed;
  }
  std::cout << "checks_passed=" << passed << '/' << results.size() << '\n';
  return passed == results.size() ? 0 : 1;
}
