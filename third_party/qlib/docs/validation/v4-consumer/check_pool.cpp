#include <qlib/pool.h>

#include <string>

bool check_pool() {
  qlib::pool<std::string> objects(1);
  const auto first = objects.emplace("first");
  const bool initial = objects[first] == "first" && objects.size() == 1 &&
      objects.capacity() >= 1;
  objects.release(first);
  const bool released = objects.empty();

  const auto replacement = objects.emplace("replacement");
  const bool reused = replacement == first && objects[replacement] == "replacement" &&
      objects.size() == 1;
  objects.release(replacement);
  return initial && released && reused && objects.empty() && objects.size() == 0;
}
