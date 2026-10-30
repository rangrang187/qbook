#include "BenchmarkCase.h"
#include "CoreTrial.h"
#include "ProtocolTrial.h"
#include "Measure.h"
#include "Results.h"
#include "Workloads.h"

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <unistd.h>

namespace {
using namespace qbook::bench;
struct Temp {
  std::filesystem::path path;
  Temp(){char name[]="/tmp/qbook-v5-check-XXXXXX";const auto* value=mkdtemp(name);if(!value)throw std::runtime_error("mkdtemp failed");path=value;}
  ~Temp(){std::error_code ec;std::filesystem::remove_all(path,ec);}
  std::filesystem::path Config(std::string_view text){auto file=path/"input.yaml";std::ofstream stream(file);stream<<text;stream.close();if(!stream)throw std::runtime_error("config fixture write failed");return file;}
};
ExperimentSpec Experiment() {ExperimentSpec e;e.experiment_id="independent-correctness";return e;}
CaseSpec SmallCore(const std::string& op) {
  CaseSpec s;s.case_id=op;s.operation=op;s.mode="Mbo";s.initial_orders=8;s.levels=2;s.events_per_trial=16;
  if(op=="core.add.growth")s.initial_orders=0;
  if(op=="core.modify.new_price"||op=="core.cancel.full")s.events_per_trial=8;
  if(op=="core.trade.full")s.events_per_trial=4;
  if(op.starts_with("core.l2.")){s.mode="L2";s.initial_orders=0;s.levels=4;if(op=="core.l2.delete")s.events_per_trial=4;}
  return s;
}
}

TEST_CASE("benchmark checked arithmetic rejects overflow", "[benchmark][config]") {
  REQUIRE(CheckedMultiply(123,456)==56088);
  REQUIRE(CheckedAdd(9007199254740993ULL,7)==9007199254741000ULL);
  REQUIRE_THROWS_AS(CheckedMultiply(UINT64_MAX,2),std::overflow_error);
  REQUIRE_THROWS_AS(CheckedAdd(UINT64_MAX,1),std::overflow_error);
}
TEST_CASE("benchmark YAML requires defined finite fields", "[benchmark][config]") {
  Temp temp;
  const std::string common="schema_version: 1\nexperiment_id: test\nrepetitions: 2\ncases:\n  - case_id: core.add.growth\n    mode: Mbo\n    levels: 2\n    events_per_trial: 8\n";
  const auto valid=LoadExperiment(temp.Config(common));REQUIRE(valid.cases.size()==1);REQUIRE(valid.repetitions==2);
  REQUIRE_THROWS(LoadExperiment(temp.Config(common+"unknown: 1\n")));
  REQUIRE_THROWS(LoadExperiment(temp.Config(common+"    batch_rows: 16\n")));
  REQUIRE_THROWS(LoadExperiment(temp.Config("schema_version: 1\nexperiment_id: bad\ncases: []\n")));
  REQUIRE_THROWS(LoadExperiment(temp.path/"missing.yaml"));
}
TEST_CASE("benchmark config rejects empty work duplicate ids and wrong mixes", "[benchmark][config]") {
  auto e=Experiment();e.cases={SmallCore("core.add.growth")};ValidateExperiment(e);
  e.cases.push_back(e.cases.front());REQUIRE_THROWS(ValidateExperiment(e));e.cases.pop_back();
  e.cases[0].events_per_trial=0;REQUIRE_THROWS(ValidateExperiment(e));
  e.cases[0].operation="writer.mixed";e.cases[0].mode="";e.cases[0].events_per_trial=10;e.cases[0].record_mix={{"order",9}};REQUIRE_THROWS(ValidateExperiment(e));
  e.cases[0].record_mix={{"order",10}};REQUIRE_NOTHROW(ValidateExperiment(e));
  e.cases[0].events_per_trial=UINT64_MAX;REQUIRE_THROWS(ValidateExperiment(e));
  CaseSpec steady;
  steady.case_id="pipeline/network-steady/mbo";steady.operation=steady.case_id;steady.mode="Mbo";
  steady.instruments=2;steady.levels=4;steady.initial_orders=32;steady.batch_rows=16;
  steady.input_rate=128;steady.warmup_sec=1;steady.window_sec=1;steady.guard_sec=2;
  steady.events_per_trial=128;steady.sample_period_ms=100;steady.stable_windows=3;
  REQUIRE_NOTHROW(ValidateCase(steady,e));
  steady.guard_sec=1;REQUIRE_THROWS(ValidateCase(steady,e));
}
TEST_CASE("benchmark CLI rejects unknown duplicate and incomplete options", "[benchmark][cli]") {
  const char* unknown[]={"bench","--not-real"};REQUIRE_THROWS(ParseOptions(2,const_cast<char**>(unknown)));
  const char* missing[]={"bench","--config"};REQUIRE_THROWS(ParseOptions(2,const_cast<char**>(missing)));
  const char* duplicate[]={"bench","--list-cases","--list-cases"};REQUIRE_THROWS(ParseOptions(3,const_cast<char**>(duplicate)));
  const char* listing[]={"bench","--list-cases"};REQUIRE(ParseOptions(2,const_cast<char**>(listing)).list_cases);
  const char* no_output[]={"bench"};REQUIRE_THROWS(ParseOptions(1,const_cast<char**>(no_output)));
}
TEST_CASE("benchmark smoke preserves actual Writer batch boundaries", "[benchmark][config]") {
  auto snapshot=SmallCore("core.snapshot");snapshot.instruments=32;snapshot.events_per_trial=4096;
  const auto small_snapshot=SmokeCase(snapshot);
  REQUIRE(small_snapshot.events_per_trial*small_snapshot.instruments<=512);
  auto reprice=SmallCore("core.modify.existing_price");reprice.initial_orders=1024;reprice.instruments=4;
  const auto small_reprice=SmokeCase(reprice);
  REQUIRE(small_reprice.initial_orders+4*small_reprice.instruments<=64);
  for(auto n:{1023ULL,1024ULL,1025ULL,2049ULL}){
    CaseSpec s;s.case_id="writer.orders";s.operation=s.case_id;s.events_per_trial=n;s.batch_rows=1024;
    const auto smoke=SmokeCase(s);REQUIRE(smoke.batch_rows==16);
    const std::uint64_t expected=n==1023?15:n==1024?16:n==1025?17:33;REQUIRE(smoke.events_per_trial==expected);
  }
  CaseSpec s;s.case_id="writer.mixed";s.operation=s.case_id;s.events_per_trial=10245;s.record_mix={{"order",2049},{"trade",2049},{"cancel",2049},{"level",2049},{"book",2049}};
  const auto smoke=SmokeCase(s);std::uint64_t sum=0;for(const auto& [key,n]:smoke.record_mix)sum+=n;REQUIRE(sum==smoke.events_per_trial);
}
TEST_CASE("benchmark smoke preserves frozen steady windows and tolerances", "[benchmark][config]") {
  CaseSpec original;
  original.case_id="pipeline/network-steady/mbo";original.operation=original.case_id;original.mode="Mbo";
  original.instruments=2;original.levels=4;original.initial_orders=32;original.batch_rows=128;
  original.input_rate=1024;original.warmup_sec=2;original.window_sec=3;original.guard_sec=1;
  original.events_per_trial=3072;original.sample_period_ms=200;original.stable_windows=4;
  original.rate_tolerance=0.125;original.lag_drift_tolerance=77;
  original.feed_book_lag_limit=128;original.book_writer_lag_limit=128;
  original.send_lateness_tolerance_ms=31;original.sample_lateness_tolerance_ms=47;
  const auto smoke=SmokeCase(original);
  REQUIRE(smoke.input_rate==128);REQUIRE(smoke.events_per_trial==384);REQUIRE(smoke.batch_rows==16);
  REQUIRE(smoke.warmup_sec==original.warmup_sec);REQUIRE(smoke.window_sec==original.window_sec);
  REQUIRE(smoke.sample_period_ms==original.sample_period_ms);REQUIRE(smoke.stable_windows==original.stable_windows);
  REQUIRE(smoke.rate_tolerance==original.rate_tolerance);REQUIRE(smoke.lag_drift_tolerance==original.lag_drift_tolerance);
  REQUIRE(smoke.feed_book_lag_limit==original.feed_book_lag_limit);REQUIRE(smoke.book_writer_lag_limit==original.book_writer_lag_limit);
  REQUIRE(smoke.send_lateness_tolerance_ms==original.send_lateness_tolerance_ms);
  REQUIRE(smoke.sample_lateness_tolerance_ms==original.sample_lateness_tolerance_ms);
  REQUIRE(smoke.guard_sec==2);REQUIRE_NOTHROW(ValidateCase(smoke,Experiment()));
  original.input_rate=64;original.events_per_trial=192;
  const auto low_rate=SmokeCase(original);
  REQUIRE(low_rate.input_rate==64);REQUIRE(low_rate.guard_sec==4);
  REQUIRE_NOTHROW(ValidateCase(low_rate,Experiment()));
}
TEST_CASE("benchmark batch executes kernel exactly once and propagates failure", "[benchmark][measure]") {
  unsigned calls=0;const auto result=MeasureBatch("apply","operations",[&]{++calls;return BatchObservation{7,9007199254740993ULL};});
  REQUIRE(calls==1);REQUIRE(result.work_count==7);REQUIRE(result.checksum==9007199254740993ULL);
  REQUIRE_THROWS(MeasureBatch("apply","operations",[]{throw std::runtime_error("kernel failed");return BatchObservation{};}));
  const auto now=Clock::now();REQUIRE(CheckedDurationNs(now,now)==0);
  REQUIRE_THROWS(CheckedDurationNs(now,now-std::chrono::nanoseconds(1)));
  if constexpr(std::ratio_equal_v<Clock::period,std::nano> && sizeof(Clock::duration::rep)==8)
    REQUIRE(CheckedDurationNs(Clock::time_point::min(),Clock::time_point::max())==UINT64_MAX);
}
TEST_CASE("benchmark pure duration and rate denominators remain distinct", "[benchmark][measure]") {
  REQUIRE_NOTHROW(ValidateMeasurement({"close","none",0,0,0},true));
  REQUIRE_THROWS(ValidateMeasurement({"close","none",1,10,0},true));
  REQUIRE_THROWS(ValidateMeasurement({"apply","operations",0,10,0},true));
  REQUIRE_THROWS(ValidateMeasurement({"apply","operations",7,0,0},true));
  REQUIRE_NOTHROW(ValidateMeasurement({"apply","operations",0,0,0},false));
}
TEST_CASE("benchmark JSON CSV preserve large integers and escape text", "[benchmark][results]") {
  REQUIRE(JsonString("中\"\\\n\t")=="\"中\\\"\\\\\\u000a\\u0009\"");
  MeasurementResult value{"run","case",0,"baseline",{"apply","operations",9007199254740993ULL,18446744073709551615ULL,9007199254740995ULL},"batch-average","valid","code","build","fnv1a64:0000000000000001","quoted, value"};
  std::ostringstream csv;WriteResult(value,csv);
  REQUIRE(csv.str().find("9007199254740993,18446744073709551615")!=std::string::npos);
  REQUIRE(csv.str().ends_with("\"quoted, value\"\n"));
  REQUIRE(std::count(kCsvHeader.begin(),kCsvHeader.end(),',')==15);
}
TEST_CASE("benchmark workload hash excludes process pointers but includes fields", "[benchmark][workload]") {
  WorkloadHasher empty;REQUIRE(empty.Finish()=="fnv1a64:cbf29ce484222325");
  auto s=SmallCore("core.add.growth");qlib::order value{};value.order_id=1;value.qty=10;
  std::vector<qlib::feed_event> events{value};const auto first=HashWorkload(s,1,{},events);
  std::get<qlib::order>(events[0]).prev=reinterpret_cast<qlib::order*>(0x1000);
  REQUIRE(HashWorkload(s,1,{},events)==first);
  ++std::get<qlib::order>(events[0]).qty;REQUIRE(HashWorkload(s,1,{},events)!=first);
  REQUIRE(HashWorkload(s,2,{},events)!=HashWorkload(s,1,{},events));
}
TEST_CASE("benchmark all core paths preserve independent state across trials", "[benchmark][core]") {
  auto e=Experiment();
  for(const auto& operation:CoreOperations()){
    INFO(operation);auto prepared=PrepareCoreBenchmark(SmallCore(operation),e);
    const auto first=prepared.run({{},false,0,"smoke"});const auto second=prepared.run({{},false,1,"smoke"});
    REQUIRE(first.validation_status=="valid");REQUIRE(second.validation_status=="valid");
    REQUIRE(first.measurements[1].work_count==second.measurements[1].work_count);
    REQUIRE(first.measurements[1].checksum==second.measurements[1].checksum);
  }
}
TEST_CASE("benchmark core preparation rejects path degeneration", "[benchmark][core]") {
  auto e=Experiment();auto s=SmallCore("core.cancel.full");s.events_per_trial=9;REQUIRE_THROWS(PrepareCoreBenchmark(s,e));
  s=SmallCore("core.trade.full");s.initial_orders=7;REQUIRE_THROWS(PrepareCoreBenchmark(s,e));
  s=SmallCore("core.modify.same_price");s.initial_orders=0;REQUIRE_THROWS(PrepareCoreBenchmark(s,e));
}
TEST_CASE("benchmark protocol validates golden fields and all rejection paths", "[benchmark][protocol]") {
  auto e=Experiment();
  for(const auto& operation:ProtocolOperations()){
    INFO(operation);CaseSpec s;s.case_id=operation;s.operation=operation;s.mode=operation=="protocol.valid.level"?"L2":"Mbo";s.working_set_frames=8;s.events_per_trial=32;
    if(operation=="protocol.mixed")s.frame_mix={{"valid_order",3},{"valid_trade",2},{"invalid_field",1},{"wrong_size",2}};
    auto prepared=PrepareProtocolBenchmark(s,e);const auto result=prepared.run({{},false,0,"smoke"});
    REQUIRE(result.validation_status=="valid");REQUIRE(result.measurements[0].work_count==32);
  }
}
