#include <qlib/pool.h>

#include <string>
#include <vector>

static_assert(__cplusplus > 202002L);

int main() {
  qlib::pool<std::string> objects(1);
  const auto first = objects.emplace("first");
  const auto before_growth = objects.capacity();
  std::vector<qlib::pool<std::string>::size_type> remaining;
  while (objects.size() < before_growth) {
    remaining.push_back(objects.emplace("extra"));
  }
  remaining.push_back(objects.emplace("last"));
  if (objects.capacity() <= before_growth || objects[first] != "first") return 1;

  objects.release(first);
  const auto replacement = objects.emplace("replacement");
  const bool correct = replacement == first && objects[replacement] == "replacement";
  objects.release(replacement);
  for (auto handle : remaining) objects.release(handle);
  return correct && objects.empty() && objects.capacity() > before_growth ? 0 : 2;
}
