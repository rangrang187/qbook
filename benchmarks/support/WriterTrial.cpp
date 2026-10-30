#include "WriterTrial.h"

#include "Measure.h"
#include "ParquetWriter.h"
#include "Workloads.h"
#include "support/ParquetReadback.h"
#include "support/ParquetWriterTestAccess.h"

#include <parquet/file_reader.h>

#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <type_traits>

namespace qbook::bench {
namespace {
constexpr std::array<const char*, 5> kKinds{"order", "trade", "cancel", "level", "book"};
constexpr std::array<const char*, 5> kFiles{
    "orders.parquet", "trades.parquet", "cancels.parquet", "levels.parquet", "books.parquet"};

struct WriterInput {
  std::vector<qlib::record> records;
  testing::ExpectedRun expected;
  std::array<std::uint64_t, 5> counts{};
  std::uint64_t batch_rows = 0;
  std::uint64_t max_output_bytes = 0;
  std::uint64_t seed = 0;
};

qlib::record MakeRecord(std::size_t kind, std::uint64_t ordinal, std::uint64_t seed) {
  const auto id = static_cast<std::int64_t>(ordinal + 1);
  const auto price = 9'007'199'254'740'993LL + static_cast<std::int64_t>(seed % 100003) + id;
  const auto event = 1'700'000'000'000'000'000LL + id;
  const auto instrument = static_cast<std::uint32_t>(ordinal % 7 + 1);
  const auto side = ordinal % 2 == 0 ? qlib::side::buy : qlib::side::sell;
  switch (kind) {
    case 0: {
      qlib::order v{};
      v.seq=id; v.order_id=id; v.price=price; v.qty=100+id; v.new_qty=ordinal%3==0 ? -id : 200+id;
      v.instrument_id=instrument; v.side=side; v.type=qlib::order_type::limit;
      v.action=ordinal%3==1 ? qlib::order_action::add : qlib::order_action::modify;
      v.event_ns=event; v.recv_ns=event+7;
      return v;
    }
    case 1: {
      qlib::trade v{};
      v.seq=id; v.buy_order_id=id; v.sell_order_id=id+1; v.price=price; v.qty=3+id;
      v.event_ns=event; v.instrument_id=instrument; v.side=side; v.recv_ns=event+7;
      return v;
    }
    case 2: {
      qlib::cancel v{};
      v.seq=id; v.order_id=id; v.qty=id+1; v.instrument_id=instrument; v.side=side;
      v.event_ns=event; v.recv_ns=event+7;
      return v;
    }
    case 3: {
      qlib::level v{};
      v.seq=id; v.price=price; v.qty=ordinal%3==0 ? -id : 300+id;
      v.event_ns=event; v.instrument_id=instrument; v.side=side; v.recv_ns=event+7;
      return v;
    }
    case 4: {
      qlib::book v{};
      v.event_ns=event; v.recv_ns=event+7; v.instrument_id=instrument;
      v.bid_price[0]=price; v.bid_qty[0]=id+100; v.ask_price[0]=price+1; v.ask_qty[0]=id+200;
      return v;
    }
    default: throw std::logic_error("writer workload: 未知记录类别");
  }
}

bool Same(const qlib::order& a, const qlib::order& b) {
  return std::tie(a.seq,a.order_id,a.price,a.qty,a.new_qty,a.prev,a.next,a.instrument_id,
      a.side,a.type,a.action,a.event_ns,a.recv_ns)==std::tie(b.seq,b.order_id,b.price,b.qty,
      b.new_qty,b.prev,b.next,b.instrument_id,b.side,b.type,b.action,b.event_ns,b.recv_ns);
}
bool Same(const qlib::trade& a, const qlib::trade& b) {
  return std::tie(a.seq,a.buy_order_id,a.sell_order_id,a.price,a.qty,a.event_ns,a.instrument_id,
      a.side,a.recv_ns)==std::tie(b.seq,b.buy_order_id,b.sell_order_id,b.price,b.qty,b.event_ns,
      b.instrument_id,b.side,b.recv_ns);
}
bool Same(const qlib::cancel& a, const qlib::cancel& b) {
  return std::tie(a.seq,a.order_id,a.qty,a.instrument_id,a.side,a.event_ns,a.recv_ns)==
      std::tie(b.seq,b.order_id,b.qty,b.instrument_id,b.side,b.event_ns,b.recv_ns);
}
bool Same(const qlib::level& a, const qlib::level& b) {
  return std::tie(a.seq,a.price,a.qty,a.event_ns,a.instrument_id,a.side,a.recv_ns)==
      std::tie(b.seq,b.price,b.qty,b.event_ns,b.instrument_id,b.side,b.recv_ns);
}
bool Same(const qlib::book& a, const qlib::book& b) {
  static_assert(qlib::snapshot_depth==1);
  return std::tie(a.event_ns,a.recv_ns,a.instrument_id,a.bid_price[0],a.bid_qty[0],
      a.ask_price[0],a.ask_qty[0])==std::tie(b.event_ns,b.recv_ns,b.instrument_id,b.bid_price[0],
      b.bid_qty[0],b.ask_price[0],b.ask_qty[0]);
}

void AddExpected(testing::ExpectedRun& expected, const qlib::record& record) {
  std::visit(qlib::overloaded{
      [&](const qlib::order& v){ expected.orders.push_back(v); },
      [&](const qlib::trade& v){ expected.trades.push_back(v); },
      [&](const qlib::cancel& v){ expected.cancels.push_back(v); },
      [&](const qlib::level& v){ expected.levels.push_back(v); },
      [&](const qlib::book& v){ expected.books.push_back(v); }}, record);
}

std::array<std::uint64_t,5> Counts(const CaseSpec& spec) {
  std::array<std::uint64_t,5> counts{};
  if (spec.operation=="writer.empty") {
    if (spec.events_per_trial!=0) throw std::invalid_argument("writer.empty 只能写零行");
  } else if (spec.operation=="writer.mixed") {
    std::uint64_t total=0;
    for (std::size_t i=0;i<kKinds.size();++i) {
      if (const auto it=spec.record_mix.find(kKinds[i]);it!=spec.record_mix.end()) counts[i]=it->second;
      total=CheckedAdd(total,counts[i]);
    }
    if (total!=spec.events_per_trial || spec.record_mix.size()!=5 || total==0)
      throw std::invalid_argument("writer.mixed 需要五类明确行数，总和等于输入数");
  } else {
    constexpr std::array<std::string_view,5> operations{
        "writer.orders","writer.trades","writer.cancels","writer.levels","writer.books"};
    bool found=false;
    for (std::size_t i=0;i<operations.size();++i) if (spec.operation==operations[i]) {
      counts[i]=spec.events_per_trial; found=true;
    }
    if (!found || spec.events_per_trial==0) throw std::invalid_argument("未知或空 Writer case");
  }
  return counts;
}

TrialResult Run(const WriterInput& input, const TrialContext& ctx,
                const detail::OutputStreamFactory& factory) {
  TrialResult result;
  const auto directory=ctx.work_dir/"parquet";
  const auto count=static_cast<std::uint64_t>(input.records.size());
  const auto kind=count ? "written_records" : "none";
  std::string stage="directory";
  std::uint64_t added=0,checksum=0;
  std::array<std::optional<Clock::time_point>,4> times;
  try {
    if (!std::filesystem::create_directory(directory))
      throw std::runtime_error("Writer trial 拒绝覆盖已有输出目录");
    // 目录创建、输入准备和验证均在四个正式写入时点之外。
    if (std::filesystem::space(directory).available<input.max_output_bytes)
      throw std::runtime_error("Writer 输出介质剩余空间低于冻结预算");
    stage="open";
    CompilerBoundary(); const auto t0=Clock::now(); times[0]=t0;
    auto writer=factory ? testing::ParquetWriterTestAccess::Create(directory,
        static_cast<std::int64_t>(input.batch_rows),factory) :
        std::make_unique<ParquetWriter>(directory,static_cast<std::int64_t>(input.batch_rows));
    const auto t1=Clock::now(); times[1]=t1; CompilerBoundary(); stage="add";
    for (const auto& record:input.records) {
      std::visit([&](const auto& v){ writer->Add(v); checksum+=static_cast<std::uint64_t>(v.event_ns); },record);
      ++added;
    }
    Consume(added); Consume(checksum); const auto t2=Clock::now(); times[2]=t2; CompilerBoundary(); stage="close";
    writer->Close(); const auto t3=Clock::now(); times[3]=t3; CompilerBoundary();
    writer.reset(); stage="readback";
    const auto verify_begin=Clock::now();
    const auto actual=testing::ReadRunDirectory(directory,static_cast<std::int64_t>(count));
    testing::CheckExpectedFiles(actual,input.expected);
    std::array<std::size_t,5> cursor{};
    for (const auto& record:input.records) {
      std::visit([&](const auto& v){
        using T=std::decay_t<decltype(v)>;
        constexpr std::size_t index=std::is_same_v<T,qlib::order>?0:std::is_same_v<T,qlib::trade>?1:
            std::is_same_v<T,qlib::cancel>?2:std::is_same_v<T,qlib::level>?3:4;
        const auto regenerated=MakeRecord(index,cursor[index]++,input.seed);
        if (!Same(v,std::get<T>(regenerated))) throw std::runtime_error("Writer 修改了原输入字段");
      },record);
    }
    if (added!=count) throw std::runtime_error("Writer 实际追加数不符");
    std::uint64_t bytes=0;
    std::array<int,5> row_groups{};
    for (std::size_t i=0;i<kFiles.size();++i) {
      auto reader=parquet::ParquetFileReader::OpenFile((directory/kFiles[i]).string(),false);
      row_groups[i]=reader->metadata()->num_row_groups();
      reader->Close();
    }
    for (const auto* file:kFiles) bytes=CheckedAdd(bytes,std::filesystem::file_size(directory/file));
    if (bytes>input.max_output_bytes) throw std::runtime_error("Writer 文件超过冻结输出预算");
    const auto verify_ns=CheckedDurationNs(verify_begin,Clock::now());
    result.measurements={{"open","none",0,CheckedDurationNs(t0,t1),checksum},
        {"add",kind,count,CheckedDurationNs(t1,t2),checksum},
        {"close","none",0,CheckedDurationNs(t2,t3),checksum},
        {"total",kind,count,CheckedDurationNs(t0,t3),checksum}};
    std::ostringstream details;
    details << "{\"added_count\":" << added << ",\"file_rows\":{";
    for (std::size_t i=0;i<5;++i) details << (i?",":"") << JsonString(kKinds[i]) << ':' << input.counts[i];
    details << "},\"physical_row_groups\":{";
    for (std::size_t i=0;i<5;++i) details << (i?",":"") << JsonString(kKinds[i]) << ':' << row_groups[i];
    details << "},\"file_bytes\":" << bytes << ",\"readback_ns\":" << verify_ns
        << ",\"readback\":\"all-fields-schema-metadata-zstd-order\",\"input_unchanged\":true"
        << ",\"parquet_directory\":" << JsonString(directory.string()) << "}";
    result.details_json=details.str();
  } catch (const std::exception& error) {
    result.validation_status="invalid";
    result.validation_message=stage+": "+error.what();
    // 故障保留工作目录和确知的成功追加量；不重试、不把未知时长写成有效成绩。
    const auto elapsed=[&](std::size_t a,std::size_t b) {
      return times[a] && times[b] ? CheckedDurationNs(*times[a],*times[b]) : 0;
    };
    result.measurements={{"open","none",0,elapsed(0,1),checksum},
        {"add",kind,added,elapsed(1,2),checksum},
        {"close","none",0,elapsed(2,3),checksum},{"total",kind,added,elapsed(0,3),checksum}};
    result.details_json="{\"failure_stage\":"+JsonString(stage)+",\"added_count\":"+
        std::to_string(added)+",\"duration_available\":{\"open\":"+(times[1]?"true":"false")+
        ",\"add\":"+(times[2]?"true":"false")+",\"close\":"+(times[3]?"true":"false")+
        ",\"total\":"+(times[3]?"true":"false")+"},\"parquet_directory\":"+
        JsonString(directory.string())+"}";
  }
  return result;
}
}  // namespace

PreparedBenchmark PrepareWriterBenchmark(const CaseSpec& spec, const ExperimentSpec& experiment,
                                         detail::OutputStreamFactory output_factory) {
  if (!spec.mode.empty() || spec.batch_rows==0 ||
      spec.batch_rows>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    throw std::invalid_argument("Writer case 不设置建簿模式，batch_rows 须为可表示正数");
  const auto bytes=CheckedMultiply(spec.events_per_trial,sizeof(qlib::record)*2);
  if (bytes>experiment.max_input_bytes || spec.events_per_trial>1'000'000)
    throw std::invalid_argument("Writer 输入超过本版冻结资源预算");
  auto input=std::make_shared<WriterInput>();
  input->counts=Counts(spec); input->batch_rows=spec.batch_rows;
  input->max_output_bytes=experiment.max_output_bytes; input->seed=experiment.seed;
  input->records.reserve(static_cast<std::size_t>(spec.events_per_trial));
  std::array<std::uint64_t,5> next{};
  while (input->records.size()<spec.events_per_trial) {
    for (std::size_t i=0;i<5;++i) if (next[i]<input->counts[i]) {
      auto record=MakeRecord(i,next[i]++,experiment.seed);
      AddExpected(input->expected,record);
      input->records.push_back(std::move(record));
    }
  }
  const auto count_kind=spec.events_per_trial ? "written_records" : "none";
  PreparedBenchmark prepared{spec,HashRecords(spec,experiment.seed,input->records),
      {{"open","none","t0 before real constructor -> t1 after constructor"},
       {"add",count_kind,"t1 -> t2 after all Add; includes dispatch and flush"},
       {"close","none","t2 -> t3 after explicit Close"},
       {"total",count_kind,"t0 -> t3; directory/readback/destruction excluded"}},
      "batch clocks; original event checksum; no per-event observer",{}};
  prepared.run=[input=std::move(input),factory=std::move(output_factory)](const TrialContext& ctx){
    return Run(*input,ctx,factory);
  };
  return prepared;
}
}  // namespace qbook::bench
