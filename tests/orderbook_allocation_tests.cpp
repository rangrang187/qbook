#include "support/AllocationFailure.h"
#include "support/OrderbookInspector.h"
#include "support/OrderbookTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>
#include <utility>
#include <vector>

namespace {

using namespace qbook;
using namespace qbook::testing;

struct FailedCall {
  AllocationReport report;
  bool bad_alloc = false;
  bool other_exception = false;
};

template <typename Call>
FailedCall FailNext(AllocationKind kind, Call&& call) {
  FailedCall outcome;
  {
    AllocationScope scope(outcome.report, kind, true);
    try {
      call();
    } catch (const std::bad_alloc&) {
      outcome.bad_alloc = true;
    } catch (...) {
      outcome.other_exception = true;
    }
  }
  return outcome;
}

void CheckHit(const FailedCall& failure, AllocationKind kind) {
  REQUIRE(failure.bad_alloc);
  REQUIRE_FALSE(failure.other_exception);
  REQUIRE(failure.report.failures == 1);
  REQUIRE(failure.report.matched_calls == 1);
  REQUIRE_FALSE(failure.report.overflow);
  REQUIRE(failure.report.count > 0);
  REQUIRE(failure.report.records[failure.report.count - 1].kind == kind);
}

void FillSix(Orderbook& book) {
  for (std::int64_t id = 1; id <= 6; ++id) {
    CheckResult(book.OnOrder(MakeOrder(id, qlib::side::buy, 100, id)),
                ApplyStatus::Applied, IgnoreReason::None);
  }
  OrderbookInspector::Check(book);
  REQUIRE(OrderbookInspector::OrderCount(book) == 6);
  REQUIRE(OrderbookInspector::IdCount(book) == 6);
  REQUIRE(OrderbookInspector::OrderCapacity(book) == 8);
  REQUIRE(OrderbookInspector::IdCapacity(book) == 8);
}

// 显式调用各重载，避免 new 表达式的合法分配消除影响工具自检。
void* AllocateForm(std::size_t form, std::size_t bytes) {
  switch (form) {
    case 0: return ::operator new(bytes);
    case 1: return ::operator new[](bytes);
    case 2: return ::operator new(bytes, std::nothrow);
    case 3: return ::operator new[](bytes, std::nothrow);
    case 4: return ::operator new(bytes, std::align_val_t{64});
    case 5: return ::operator new[](bytes, std::align_val_t{64});
    case 6: return ::operator new(bytes, std::align_val_t{64}, std::nothrow);
    case 7: return ::operator new[](bytes, std::align_val_t{64}, std::nothrow);
    default: std::terminate();
  }
}

void DeallocateForm(std::size_t form, void* pointer) noexcept {
  switch (form) {
    case 0: ::operator delete(pointer); break;
    case 1: ::operator delete[](pointer); break;
    case 2: ::operator delete(pointer, std::nothrow); break;
    case 3: ::operator delete[](pointer, std::nothrow); break;
    case 4: ::operator delete(pointer, std::align_val_t{64}); break;
    case 5: ::operator delete[](pointer, std::align_val_t{64}); break;
    case 6: ::operator delete(pointer, std::align_val_t{64}, std::nothrow); break;
    case 7: ::operator delete[](pointer, std::align_val_t{64}, std::nothrow); break;
    default: std::terminate();
  }
}

}  // namespace

TEST_CASE("Allocation replacements cover ordinary aligned array and nothrow forms",
          "[allocation][allocation-helper]") {
  for (std::size_t form = 0; form < 8; ++form) {
    INFO("allocation form " << form);
    const auto kind = form < 4 ? AllocationKind::Ordinary : AllocationKind::Aligned;
    const bool nothrow = form == 2 || form == 3 || form == 6 || form == 7;
    AllocationReport probe;
    void* allocated = nullptr;
    {
      AllocationScope scope(probe);
      allocated = AllocateForm(form, 0);
    }
    const auto address = reinterpret_cast<std::uintptr_t>(allocated);
    DeallocateForm(form, allocated);
    REQUIRE(allocated != nullptr);
    REQUIRE(probe.count == 1);
    REQUIRE(probe.records[0].kind == kind);
    REQUIRE(probe.records[0].bytes == 0);
    REQUIRE(probe.failures == 0);
    if (kind == AllocationKind::Aligned) REQUIRE(address % 64 == 0);

    AllocationReport failure;
    bool caught = false;
    void* failed = nullptr;
    void* after_hit = nullptr;
    {
      AllocationScope scope(failure, kind, true);
      try {
        failed = AllocateForm(form, 32);
      } catch (const std::bad_alloc&) {
        caught = true;
      }
      after_hit = AllocateForm(form, 32);
    }
    DeallocateForm(form, failed);
    DeallocateForm(form, after_hit);
    REQUIRE(caught == !nothrow);
    REQUIRE(failed == nullptr);
    REQUIRE(after_hit != nullptr);
    REQUIRE(failure.failures == 1);
    REQUIRE(failure.count == 2);
    REQUIRE_FALSE(failure.overflow);
  }

  // sized delete 与对应分配共享 free 路径；普通及 aligned 数组分别检查。
  ::operator delete(::operator new(24), std::size_t{24});
  ::operator delete[](::operator new[](24), std::size_t{24});
  ::operator delete(::operator new(24, std::align_val_t{64}),
                    std::size_t{24}, std::align_val_t{64});
  ::operator delete[](::operator new[](24, std::align_val_t{64}),
                      std::size_t{24}, std::align_val_t{64});
}

TEST_CASE("F01 new price level allocation failure preserves an empty MBO book",
          "[allocation][orderbook][F01]") {
  auto event = MakeOrder(1, qlib::side::buy, 100, 10);
  event.prev = &event;
  event.next = &event;
  const auto input = event;
  Orderbook probe_book(1, BookMode::Mbo);
  AllocationReport probe;
  ApplyResult applied{};
  {
    AllocationScope scope(probe);
    applied = probe_book.OnOrder(event);
  }
  CheckResult(applied, ApplyStatus::Applied, IgnoreReason::None);
  REQUIRE(probe.ordinary_calls == 1);
  REQUIRE(probe.aligned_calls == 2);
  REQUIRE(probe.count == 3);
  REQUIRE(probe.records[0].kind == AllocationKind::Ordinary);
  OrderbookInspector::Check(probe_book);

  Orderbook book(1, BookMode::Mbo);
  const auto before = OrderbookInspector::SnapshotState(book);
  const auto failure = FailNext(AllocationKind::Ordinary, [&] { book.OnOrder(event); });
  CheckHit(failure, AllocationKind::Ordinary);
  REQUIRE(failure.report.count == 1);
  REQUIRE(failure.report.records[0] == probe.records[0]);
  REQUIRE(OrderbookInspector::SnapshotState(book) == before);
  REQUIRE(OrderbookInspector::OrderCount(book) == 0);
  REQUIRE(OrderbookInspector::IdCount(book) == 0);
  REQUIRE(OrderbookInspector::LevelCount(book, qlib::side::buy) == 0);
  CheckInput(event, input);
  OrderbookInspector::Check(book);

  CheckResult(book.OnOrder(event), ApplyStatus::Applied, IgnoreReason::None);
  CheckSnapshot(book, 100, 10, 0, 0, event.event_ns, event.recv_ns);
  REQUIRE(book.size() == 1);
  CheckInput(event, input);
  OrderbookInspector::Check(book);
}

TEST_CASE("F02 hive allocation failure removes the prepared empty price level",
          "[allocation][orderbook][F02]") {
  auto event = MakeOrder(1, qlib::side::buy, 100, 10);
  event.prev = &event;
  event.next = &event;
  const auto input = event;
  Orderbook probe_book(1, BookMode::Mbo);
  AllocationReport probe;
  {
    AllocationScope scope(probe);
    probe_book.OnOrder(event);
  }
  REQUIRE(probe.count == 3);
  REQUIRE(probe.ordinary_calls == 1);
  REQUIRE(probe.aligned_calls == 2);
  REQUIRE(probe.records[0].kind == AllocationKind::Ordinary);
  REQUIRE(probe.records[1].kind == AllocationKind::Aligned);
  REQUIRE(probe.records[1].bytes > 8 * sizeof(qlib::order));
  REQUIRE(probe.records[2].bytes ==
          8 * sizeof(std::pair<std::int64_t, qlib::order*>) + 9);
  REQUIRE(OrderbookInspector::OrderCapacity(probe_book) == 8);
  REQUIRE(OrderbookInspector::IdCapacity(probe_book) == 8);
  OrderbookInspector::Check(probe_book);

  Orderbook book(1, BookMode::Mbo);
  const auto before = OrderbookInspector::SnapshotState(book);
  const auto failure = FailNext(AllocationKind::Aligned, [&] { book.OnOrder(event); });
  CheckHit(failure, AllocationKind::Aligned);
  REQUIRE(failure.report.count == 2);
  REQUIRE(failure.report.records[0] == probe.records[0]);
  REQUIRE(failure.report.records[1] == probe.records[1]);
  REQUIRE(OrderbookInspector::SnapshotState(book) == before);
  REQUIRE(OrderbookInspector::OrderCount(book) == 0);
  REQUIRE(OrderbookInspector::IdCount(book) == 0);
  REQUIRE(OrderbookInspector::LevelCount(book, qlib::side::buy) == 0);
  CheckInput(event, input);
  OrderbookInspector::Check(book);

  CheckResult(book.OnOrder(event), ApplyStatus::Applied, IgnoreReason::None);
  REQUIRE(OrderbookInspector::OrderCount(book) == 1);
  REQUIRE(OrderbookInspector::IdCount(book) == 1);
  CheckSnapshot(book, 100, 10, 0, 0, event.event_ns, event.recv_ns);
  CheckInput(event, input);
  OrderbookInspector::Check(book);
}

TEST_CASE("F03 ID growth failure removes the seventh hive object and preserves FIFO",
          "[allocation][orderbook][F03]") {
  const auto event = MakeOrder(7, qlib::side::buy, 100, 7);
  const auto input = event;
  Orderbook probe_book(1, BookMode::Mbo);
  FillSix(probe_book);
  AllocationReport probe;
  {
    AllocationScope scope(probe);
    probe_book.OnOrder(event);
  }
  REQUIRE(probe.count == 1);
  REQUIRE(probe.ordinary_calls == 0);
  REQUIRE(probe.aligned_calls == 1);
  REQUIRE(probe.records[0].kind == AllocationKind::Aligned);
  REQUIRE(probe.records[0].bytes ==
          16 * sizeof(std::pair<std::int64_t, qlib::order*>) + 17);
  REQUIRE(OrderbookInspector::OrderCapacity(probe_book) == 8);
  REQUIRE(OrderbookInspector::IdCapacity(probe_book) == 16);
  REQUIRE(OrderbookInspector::OrderCount(probe_book) == 7);
  OrderbookInspector::Check(probe_book);

  Orderbook book(1, BookMode::Mbo);
  FillSix(book);
  const auto before = OrderbookInspector::SnapshotState(book);
  const auto failure = FailNext(AllocationKind::Aligned, [&] { book.OnOrder(event); });
  CheckHit(failure, AllocationKind::Aligned);
  REQUIRE(failure.report.count == 1);
  REQUIRE(failure.report.records[0] == probe.records[0]);
  REQUIRE(OrderbookInspector::SnapshotState(book) == before);
  REQUIRE(OrderbookInspector::OrderCount(book) == 6);
  REQUIRE(OrderbookInspector::IdCount(book) == 6);
  REQUIRE(OrderbookInspector::Node(book, 7) == nullptr);
  REQUIRE(book.OnSnapshot().bid_qty[0] == 21);
  CheckInput(event, input);
  OrderbookInspector::Check(book);

  CheckResult(book.OnOrder(event), ApplyStatus::Applied, IgnoreReason::None);
  REQUIRE(OrderbookInspector::OrderCount(book) == 7);
  REQUIRE(OrderbookInspector::IdCount(book) == 7);
  REQUIRE(OrderbookInspector::IdCapacity(book) == 16);
  REQUIRE(OrderbookInspector::Fifo(book, qlib::side::buy, 100) ==
          std::vector<std::int64_t>{1, 2, 3, 4, 5, 6, 7});
  CheckSnapshot(book, 100, 28, 0, 0, event.event_ns, event.recv_ns);
  CheckInput(event, input);
  OrderbookInspector::Check(book);
}

TEST_CASE("F04 repricing allocation failure leaves the original order and level intact",
          "[allocation][orderbook][F04]") {
  const auto first = MakeOrder(1, qlib::side::buy, 100, 10);
  const auto second = MakeOrder(2, qlib::side::buy, 100, 5);
  const auto event = MakeModify(1, qlib::side::buy, 101, 6);
  const auto input = event;
  Orderbook probe_book(1, BookMode::Mbo);
  probe_book.OnOrder(first);
  probe_book.OnOrder(second);
  AllocationReport probe;
  {
    AllocationScope scope(probe);
    probe_book.OnOrder(event);
  }
  REQUIRE(probe.count == 1);
  REQUIRE(probe.ordinary_calls == 1);
  REQUIRE(probe.aligned_calls == 0);
  REQUIRE(probe.records[0].kind == AllocationKind::Ordinary);
  OrderbookInspector::Check(probe_book);

  Orderbook book(1, BookMode::Mbo);
  book.OnOrder(first);
  book.OnOrder(second);
  const auto before = OrderbookInspector::SnapshotState(book);
  const auto* original = OrderbookInspector::Node(book, 1);
  const auto failure = FailNext(AllocationKind::Ordinary, [&] { book.OnOrder(event); });
  CheckHit(failure, AllocationKind::Ordinary);
  REQUIRE(failure.report.count == 1);
  REQUIRE(failure.report.records[0] == probe.records[0]);
  REQUIRE(OrderbookInspector::SnapshotState(book) == before);
  REQUIRE(OrderbookInspector::LevelCount(book, qlib::side::buy) == 1);
  REQUIRE(OrderbookInspector::Node(book, 1) == original);
  CheckInput(event, input);
  OrderbookInspector::Check(book);

  CheckResult(book.OnOrder(event), ApplyStatus::Applied, IgnoreReason::None);
  REQUIRE(OrderbookInspector::Node(book, 1) == original);
  REQUIRE(OrderbookInspector::Fifo(book, qlib::side::buy, 100) ==
          std::vector<std::int64_t>{2});
  REQUIRE(OrderbookInspector::Fifo(book, qlib::side::buy, 101) ==
          std::vector<std::int64_t>{1});
  CheckSnapshot(book, 101, 6, 0, 0, event.event_ns, event.recv_ns);
  CheckInput(event, input);
  OrderbookInspector::Check(book);
}

TEST_CASE("F05 L2 allocation failure preserves every existing level without orders",
          "[allocation][orderbook][F05]") {
  const auto bid = MakeLevel(qlib::side::buy, 99, 20);
  const auto ask = MakeLevel(qlib::side::sell, 102, 30);
  const auto event = MakeLevel(qlib::side::buy, 100, 8);
  const auto input = event;
  Orderbook probe_book(1, BookMode::L2);
  probe_book.OnLevel(bid);
  probe_book.OnLevel(ask);
  AllocationReport probe;
  {
    AllocationScope scope(probe);
    probe_book.OnLevel(event);
  }
  REQUIRE(probe.count == 1);
  REQUIRE(probe.ordinary_calls == 1);
  REQUIRE(probe.aligned_calls == 0);
  REQUIRE(probe.records[0].kind == AllocationKind::Ordinary);
  OrderbookInspector::Check(probe_book);

  Orderbook book(1, BookMode::L2);
  book.OnLevel(bid);
  book.OnLevel(ask);
  const auto before = OrderbookInspector::SnapshotState(book);
  const auto failure = FailNext(AllocationKind::Ordinary, [&] { book.OnLevel(event); });
  CheckHit(failure, AllocationKind::Ordinary);
  REQUIRE(failure.report.count == 1);
  REQUIRE(failure.report.records[0] == probe.records[0]);
  REQUIRE(OrderbookInspector::SnapshotState(book) == before);
  REQUIRE(OrderbookInspector::OrderCount(book) == 0);
  REQUIRE(OrderbookInspector::IdCount(book) == 0);
  REQUIRE(OrderbookInspector::LevelCount(book, qlib::side::buy) == 1);
  CheckInput(event, input);
  OrderbookInspector::Check(book);

  CheckResult(book.OnLevel(event), ApplyStatus::Applied, IgnoreReason::None);
  REQUIRE(book.size() == 0);
  REQUIRE(OrderbookInspector::LevelCount(book, qlib::side::buy) == 2);
  CheckSnapshot(book, 100, 8, 102, 30, event.event_ns, event.recv_ns);
  CheckInput(event, input);
  OrderbookInspector::Check(book);
}
