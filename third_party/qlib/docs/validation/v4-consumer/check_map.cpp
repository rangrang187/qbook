#include <qlib/map.h>

bool check_map() {
  qlib::map<int, int> values;
  const auto inserted = values.try_emplace(7, 70);
  if (!inserted.second || inserted.first == values.end() || inserted.first->second != 70) {
    return false;
  }

  const auto duplicate = values.try_emplace(7, 99);
  if (duplicate.second || duplicate.first == values.end() || duplicate.first->second != 70) {
    return false;
  }
  const auto found = values.find(7);
  if (found == values.end() || found->first != 7 || found->second != 70 || values.size() != 1) {
    return false;
  }

  const auto erased = values.erase(7);
  const bool absent = values.find(7) == values.end();
  values.clear();
  return erased == 1 && absent && values.empty();
}
