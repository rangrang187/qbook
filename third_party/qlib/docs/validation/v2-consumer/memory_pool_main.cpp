#include <qlib/memory_pool.h>

#include <cstdint>
#include <memory>
#include <string>

static_assert(__cplusplus > 202002L);

int main() {
  qlib::memory_pool storage(sizeof(std::string), 1, alignof(std::string));
  void* raw = storage.allocate();
  if (raw == nullptr) return 1;

  std::string* value = nullptr;
  try {
    value = std::construct_at(static_cast<std::string*>(raw), "hello");
  } catch (...) {
    storage.deallocate(raw);
    throw;
  }

  const bool correct = *value == "hello" && storage.size() == 1 &&
      storage.capacity() == 1 && storage.allocate() == nullptr &&
      storage.block_size() >= sizeof(std::string) &&
      reinterpret_cast<std::uintptr_t>(raw) % storage.alignment() == 0;
  // 对象自身的资源先清理，归还随后才可覆盖块头。
  std::destroy_at(value);
  storage.deallocate(raw);
  if (!correct || !storage.empty()) return 2;

  void* reused = storage.allocate();
  if (reused == nullptr) return 3;
  const bool same_block = reused == raw;
  storage.deallocate(reused);
  return same_block && storage.empty() ? 0 : 4;
}
