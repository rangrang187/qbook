#include "AllocationFailure.h"

#include <cstdlib>
#include <exception>
#include <new>

namespace {

using qbook::testing::AllocationKind;
using qbook::testing::AllocationReport;

struct AllocationState {
  AllocationReport* report = nullptr;
  AllocationKind filter = AllocationKind::Any;
  bool fail_next = false;
};

constinit thread_local AllocationState state;

bool Record(AllocationKind kind, std::size_t bytes, std::size_t alignment) noexcept {
  if (state.report == nullptr) return false;
  auto& report = *state.report;
  if (report.count < report.records.size()) {
    report.records[report.count++] = {kind, bytes, alignment};
  } else {
    report.overflow = true;
  }
  if (kind == AllocationKind::Ordinary) ++report.ordinary_calls;
  else ++report.aligned_calls;
  if (state.filter != AllocationKind::Any && state.filter != kind) return false;
  ++report.matched_calls;
  if (!state.fail_next) return false;
  state.fail_next = false;
  ++report.failures;
  return true;
}

void* Allocate(std::size_t bytes, AllocationKind kind, std::size_t alignment = 0) {
  if (Record(kind, bytes, alignment)) throw std::bad_alloc{};
  const auto requested = bytes == 0 ? std::size_t{1} : bytes;
  // posix_memalign 要求对齐至少为指针大小，qlib 也会显式请求较小对齐。
  const auto actual_alignment = alignment < sizeof(void*) ? sizeof(void*) : alignment;
  for (;;) {
    void* memory = nullptr;
    if (kind == AllocationKind::Ordinary) {
      memory = std::malloc(requested);
    } else if (::posix_memalign(&memory, actual_alignment, requested) != 0) {
      memory = nullptr;
    }
    if (memory != nullptr) return memory;
    const auto handler = std::get_new_handler();
    if (handler == nullptr) throw std::bad_alloc{};
    handler();
  }
}

void* AllocateNoThrow(std::size_t bytes, AllocationKind kind,
                      std::size_t alignment = 0) noexcept {
  try {
    return Allocate(bytes, kind, alignment);
  } catch (...) {
    return nullptr;
  }
}

}  // namespace

namespace qbook::testing {

AllocationScope::AllocationScope(AllocationReport& report, AllocationKind kind,
                                 bool fail_next) noexcept {
  if (state.report != nullptr) std::terminate();
  report = {};
  state = {&report, kind, fail_next};
}

AllocationScope::~AllocationScope() { state = {}; }

}  // namespace qbook::testing

// 全部分配替换只链接进 qbook_allocation_tests。
void* operator new(std::size_t bytes) {
  return Allocate(bytes, AllocationKind::Ordinary);
}

void* operator new[](std::size_t bytes) {
  return Allocate(bytes, AllocationKind::Ordinary);
}

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
  return AllocateNoThrow(bytes, AllocationKind::Ordinary);
}

void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
  return AllocateNoThrow(bytes, AllocationKind::Ordinary);
}

void* operator new(std::size_t bytes, std::align_val_t alignment) {
  return Allocate(bytes, AllocationKind::Aligned, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t bytes, std::align_val_t alignment) {
  return Allocate(bytes, AllocationKind::Aligned, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t bytes, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
  return AllocateNoThrow(bytes, AllocationKind::Aligned, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t bytes, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
  return AllocateNoThrow(bytes, AllocationKind::Aligned, static_cast<std::size_t>(alignment));
}

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
  std::free(pointer);
}
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
  std::free(pointer);
}
