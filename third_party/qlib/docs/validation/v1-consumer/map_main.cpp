#include <qlib/map.h>

static_assert(__cplusplus > 202002L);

int main() {
  qlib::map<int, int> values;
  values.reserve(100);
  for (int i = 0; i < 100; ++i) {
    if (!values.try_emplace(int{i}, i * 2).second) {
      return 1;
    }
  }
  if (values.try_emplace(42, -1).second || values.at(42) != 84) {
    return 2;
  }
  if (values.erase(42) != 1 || values.contains(42)) {
    return 3;
  }
  int sum = 0;
  for (const auto& item : values) {
    sum += item.second;
  }
  if (values.size() != 99 || sum != 9816) {
    return 4;
  }
  values.clear();
  return values.empty() && values.capacity() == 0 ? 0 : 5;
}
