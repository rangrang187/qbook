#include <qlib/memory_pool.h>

#include <cstdint>
#include <memory>
#include <string>

bool check_memory_pool() {
  qlib::memory_pool storage(sizeof(std::string), 1, alignof(std::string));
  if (storage.block_size() < sizeof(std::string) ||
      storage.alignment() < alignof(std::string) || storage.capacity() != 1) {
    return false;
  }
  void* raw = storage.allocate();
  if (raw == nullptr) return false;
  if (reinterpret_cast<std::uintptr_t>(raw) % storage.alignment() != 0) {
    storage.deallocate(raw);
    return false;
  }

  void* extra = storage.allocate();
  if (extra != nullptr) {
    storage.deallocate(extra);
    storage.deallocate(raw);
    return false;
  }

  std::string* value = nullptr;
  try {
    value = std::construct_at(static_cast<std::string*>(raw), 64, 'x');
  } catch (...) {
    // 构造失败时仅归还原始块，异常继续交给入口报告。
    storage.deallocate(raw);
    throw;
  }

  bool correct = false;
  try {
    correct = value->size() == 64 && value->front() == 'x' && storage.size() == 1;
  } catch (...) {
    // 已构造对象先析构，归还随后才可覆盖块头。
    std::destroy_at(value);
    storage.deallocate(raw);
    throw;
  }
  std::destroy_at(value);
  storage.deallocate(raw);
  if (!correct || !storage.empty()) return false;

  void* reused = storage.allocate();
  if (reused == nullptr) return false;
  const bool same_block = reused == raw && storage.size() == 1;
  storage.deallocate(reused);
  return same_block && storage.empty() && storage.size() == 0;
}
