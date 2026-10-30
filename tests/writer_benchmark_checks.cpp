#include "WriterTrial.h"
#include "support/FailingOutputStream.h"
#include "support/StorageTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <fstream>
#include <limits>

namespace {
using namespace qbook::bench;
using namespace qbook::testing;

CaseSpec Spec(std::string operation, std::uint64_t count, std::uint64_t batch = 4) {
  CaseSpec result;
  result.case_id = operation;
  result.operation = std::move(operation);
  result.events_per_trial = count;
  result.batch_rows = batch;
  return result;
}

TrialResult Run(const PreparedBenchmark& prepared, const std::filesystem::path& path) {
  return prepared.run({path, false, 0, "smoke"});
}

void RequireValid(const TrialResult& result, std::uint64_t count) {
  INFO(result.validation_message);
  REQUIRE(result.validation_status == "valid");
  REQUIRE(result.measurements.size() == 4);
  const std::array<std::string, 4> names{"open", "add", "close", "total"};
  for (std::size_t i = 0; i != 4; ++i) {
    const auto& phase = result.measurements[i];
    REQUIRE(phase.phase == names[i]);
    REQUIRE(phase.duration_ns > 0);
    const bool counted = count != 0 && (i == 1 || i == 3);
    REQUIRE(phase.count_kind == (counted ? "written_records" : "none"));
    REQUIRE(phase.work_count == (counted ? count : 0));
  }
  REQUIRE(result.measurements[3].duration_ns == result.measurements[0].duration_ns +
      result.measurements[1].duration_ns + result.measurements[2].duration_ns);
  REQUIRE(result.details_json.find("all-fields-schema-metadata-zstd-order") != std::string::npos);
}
}

TEST_CASE("Writer benchmark validates all five types at every batch boundary", "[benchmark][writer]") {
  ExperimentSpec experiment;
  for (const auto* operation : {"writer.orders", "writer.trades", "writer.cancels", "writer.levels", "writer.books"}) {
    for (const auto count : {3U, 4U, 5U, 9U}) {
      CAPTURE(operation, count);
      TempDirectory directory;
      const auto prepared = PrepareWriterBenchmark(Spec(operation, count), experiment);
      RequireValid(Run(prepared, directory.path()), count);
    }
  }
  TempDirectory empty;
  RequireValid(Run(PrepareWriterBenchmark(Spec("writer.empty", 0), experiment), empty.path()), 0);
}

TEST_CASE("Writer benchmark mixed types keep independent batches and original input", "[benchmark][writer]") {
  auto spec = Spec("writer.mixed", 24);
  spec.record_mix = {{"order", 3}, {"trade", 4}, {"cancel", 5}, {"level", 9}, {"book", 3}};
  const auto prepared = PrepareWriterBenchmark(spec, ExperimentSpec{});
  TempDirectory first, second;
  const auto a = Run(prepared, first.path());
  const auto b = Run(prepared, second.path());
  RequireValid(a, 24); RequireValid(b, 24);
  REQUIRE(a.measurements[1].checksum == b.measurements[1].checksum);
  REQUIRE(a.details_json.find("\"input_unchanged\":true") != std::string::npos);
}

TEST_CASE("Writer benchmark rejects invalid resource and workload parameters", "[benchmark][writer]") {
  ExperimentSpec experiment;
  auto spec = Spec("writer.orders", 3);
  spec.batch_rows = 0;
  REQUIRE_THROWS(PrepareWriterBenchmark(spec, experiment));
  spec.batch_rows = std::numeric_limits<std::uint64_t>::max();
  REQUIRE_THROWS(PrepareWriterBenchmark(spec, experiment));
  spec = Spec("writer.orders", 3); experiment.max_input_bytes = 1;
  REQUIRE_THROWS(PrepareWriterBenchmark(spec, experiment));
  experiment.max_input_bytes = 100000;
  spec = Spec("writer.mixed", 3);
  spec.record_mix = {{"order", 3}};
  REQUIRE_THROWS(PrepareWriterBenchmark(spec, experiment));
}

TEST_CASE("Writer benchmark refuses existing output without overwriting evidence", "[benchmark][writer]") {
  TempDirectory directory;
  std::filesystem::create_directory(directory.path() / "parquet");
  const auto file = directory.path() / "parquet" / "orders.parquet";
  { std::ofstream output(file); output << "existing evidence"; }
  const auto result = Run(PrepareWriterBenchmark(Spec("writer.orders", 1), ExperimentSpec{}), directory.path());
  REQUIRE(result.validation_status == "invalid");
  REQUIRE(result.validation_message.starts_with("directory:"));
  std::ifstream input(file); std::string text; std::getline(input, text);
  REQUIRE(text == "existing evidence");
}

TEST_CASE("Writer benchmark partial open failures cannot become valid results", "[benchmark][writer]") {
  TempDirectory directory;
  auto probe = std::make_shared<StreamProbe>();
  StreamFaultPlan plan; plan.file = "levels.parquet"; plan.fail_open = true;
  const auto result = Run(PrepareWriterBenchmark(Spec("writer.orders", 4), ExperimentSpec{},
      MakeOutputFactory(probe, plan)), directory.path());
  REQUIRE(result.validation_status == "invalid");
  REQUIRE(result.validation_message.starts_with("open:"));
  REQUIRE(probe->failures == 1);
}

TEST_CASE("Writer benchmark invalidates actual full batch tail and footer write failures", "[benchmark][writer]") {
  // 全部真实流创建后才武装，故障命中数据/尾批/footer，不混入文件头打开。
  for (const auto count : {0U, 3U, 4U}) {
    CAPTURE(count);
    TempDirectory directory;
    auto probe = std::make_shared<StreamProbe>();
    StreamFaultPlan plan; plan.fail_write = true;
    auto base = MakeOutputFactory(probe, plan);
    auto factory = [base, probe, opened = 0U](const std::filesystem::path& path) mutable {
      auto stream = base(path);
      if (++opened == 5) probe->armed.store(true);
      return stream;
    };
    const auto result = Run(PrepareWriterBenchmark(Spec(count ? "writer.orders" : "writer.empty", count),
        ExperimentSpec{}, factory), directory.path());
    REQUIRE(probe->entered.load());
    REQUIRE(probe->failures == 1);
    REQUIRE(result.validation_status == "invalid");
    REQUIRE(result.validation_message.starts_with(count == 4 ? "add:" : "close:"));
    REQUIRE(std::filesystem::exists(directory.path() / "parquet" / "orders.parquet"));
  }
}

TEST_CASE("Writer benchmark Close failure remains invalid after successful Adds", "[benchmark][writer]") {
  TempDirectory directory;
  auto probe = std::make_shared<StreamProbe>();
  StreamFaultPlan plan; plan.fail_close = true;
  const auto result = Run(PrepareWriterBenchmark(Spec("writer.orders", 5), ExperimentSpec{},
      MakeOutputFactory(probe, plan)), directory.path());
  REQUIRE(probe->failures == 1);
  REQUIRE(result.validation_status == "invalid");
  REQUIRE(result.validation_message.starts_with("close:"));
  REQUIRE(result.details_json.find("\"added_count\":5") != std::string::npos);
}
