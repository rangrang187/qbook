#include <qlib/hive.h>

#include <memory>

bool check_hive() {
  qlib::hive<int> values;
  const auto keep = values.emplace(7);
  if (keep == values.end()) return false;
  const int* address = std::addressof(*keep);
  const auto remove = values.emplace(9);
  if (remove == values.end()) return false;
  for (int index = 0; index < 16; ++index) {
    values.emplace(100 + index);
  }
  values.erase(remove);

  const auto found = values.get_iterator(address);
  const bool stable = found != values.end() && found == keep &&
      std::addressof(*found) == address && *found == 7 && values.size() == 17;
  values.clear();
  return stable && values.empty() && values.size() == 0 && values.capacity() == 0;
}
