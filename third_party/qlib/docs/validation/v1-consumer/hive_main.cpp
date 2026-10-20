#include <qlib/hive.h>

static_assert(__cplusplus > 202002L);

int main() {
  qlib::hive<int> values;
  auto keep = values.emplace(7);
  const int* address = &*keep;
  auto remove = values.emplace(9);
  for (int i = 0; i < 100; ++i) {
    values.emplace(100 + i);
  }
  values.erase(remove);
  if (*address != 7 || values.get_iterator(address) != keep) {
    return 1;
  }
  int sum = 0;
  for (auto it = values.rbegin(); it != values.rend(); ++it) {
    sum += *it;
  }
  if (values.size() != 101 || sum != 14957) {
    return 2;
  }
  values.clear();
  return values.empty() && values.capacity() == 0 ? 0 : 3;
}
