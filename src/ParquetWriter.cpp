#include "ParquetWriter.h"

#include "PipelineTypes.h"
#include "detail/OutputStreams.h"

#include <arrow/api.h>
#include <arrow/io/interfaces.h>
#include <arrow/util/compression.h>
#include <parquet/arrow/writer.h>
#include <spdlog/spdlog.h>

#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qbook {
namespace {
void Check(const arrow::Status& status, const std::string& file, const char* operation) {
  if (!status.ok()) throw std::runtime_error(file + " / " + operation + ": " + status.ToString());
}
template <class T>
T Take(arrow::Result<T> result, const std::string& file, const char* operation) {
  Check(result.status(), file, operation);
  return std::move(result).ValueOrDie();
}
void CleanupDiagnostic(const std::string& file, std::string_view message) noexcept {
  try { spdlog::error("{} / resource cleanup: {}", file, message); } catch (...) {}
}
void CheckCapacity(std::int64_t batch_rows) {
  constexpr auto depth = qlib::snapshot_depth;
  static_assert(depth > 0);
  if (batch_rows <= 0 || batch_rows > std::numeric_limits<std::int64_t>::max() / depth)
    throw std::invalid_argument("batch_rows: 行数与列表元素数超出范围");
  const auto elements = batch_rows * depth;
  constexpr auto max_bytes = std::numeric_limits<std::int64_t>::max();
  if (!std::in_range<std::size_t>(elements) ||
      elements > max_bytes / static_cast<std::int64_t>(sizeof(std::int64_t)) ||
      static_cast<std::size_t>(elements) > std::numeric_limits<std::size_t>::max() / sizeof(std::int64_t))
    throw std::invalid_argument("batch_rows: Arrow 列字节数超出范围");
}

std::shared_ptr<arrow::Schema> Schema(std::vector<std::shared_ptr<arrow::Field>> fields) {
  return arrow::schema(std::move(fields), arrow::key_value_metadata(
      {"qbook.format_version", "qbook.price_scale", "qbook.qty_scale", "qbook.time_unit",
       "qbook.snapshot_depth", "qbook.side", "qbook.order_type", "qbook.order_action"},
      {"1", std::to_string(qlib::price_scale), std::to_string(qlib::qty_scale), "unix_epoch_ns",
       std::to_string(qlib::snapshot_depth), "0:buy,1:sell", "0:limit,1:market", "0:add,1:modify"}));
}
std::shared_ptr<arrow::Field> I64(const char* name) { return arrow::field(name, arrow::int64(), false); }
std::shared_ptr<arrow::Field> U32(const char* name) { return arrow::field(name, arrow::uint32(), false); }
std::shared_ptr<arrow::Field> U8(const char* name) { return arrow::field(name, arrow::uint8(), false); }
std::shared_ptr<arrow::Field> List(const char* name) {
  return arrow::field(name, arrow::fixed_size_list(
      arrow::field("item", arrow::int64(), false), qlib::snapshot_depth), false);
}
}

struct ParquetWriter::Sink {
  std::string file;
  std::shared_ptr<arrow::Schema> schema;
  std::vector<std::shared_ptr<arrow::ArrayBuilder>> builders;
  std::shared_ptr<arrow::io::OutputStream> output;
  std::unique_ptr<parquet::arrow::FileWriter> writer;
  std::int64_t batch_rows = 0;
  std::int64_t rows = 0;

  ~Sink() noexcept { Cleanup(); }
  void Cleanup() noexcept {
    // 先禁止底层写入，随后释放可能在析构中尝试写 footer 的 Parquet writer。
    if (output) {
      try {
        if (!output->closed()) {
          const auto status = output->Abort();
          if (!status.ok()) CleanupDiagnostic(file, status.ToString());
        }
      } catch (const std::exception& error) { CleanupDiagnostic(file, error.what()); }
        catch (...) { CleanupDiagnostic(file, "未知 Abort 异常"); }
    }
    writer.reset();
    output.reset();
  }
  arrow::Int64Builder& I(std::size_t index) {
    return static_cast<arrow::Int64Builder&>(*builders[index]);
  }
  arrow::UInt32Builder& U32(std::size_t index) {
    return static_cast<arrow::UInt32Builder&>(*builders[index]);
  }
  arrow::UInt8Builder& U8(std::size_t index) {
    return static_cast<arrow::UInt8Builder&>(*builders[index]);
  }
};

ParquetWriter::ParquetWriter(const std::filesystem::path& run_dir, std::int64_t batch_rows)
    : ParquetWriter(run_dir, batch_rows, detail::OpenExclusiveOutput) {}

ParquetWriter::ParquetWriter(const std::filesystem::path& run_dir, std::int64_t batch_rows,
                             const Factory& output_factory) {
  CheckCapacity(batch_rows);
  if (!arrow::util::Codec::IsAvailable(arrow::Compression::ZSTD))
    throw std::runtime_error("Parquet / Open: ZSTD 不可用");
  orders_ = OpenOrders(run_dir / "orders.parquet", batch_rows, output_factory);
  trades_ = OpenTrades(run_dir / "trades.parquet", batch_rows, output_factory);
  cancels_ = OpenCancels(run_dir / "cancels.parquet", batch_rows, output_factory);
  levels_ = OpenLevels(run_dir / "levels.parquet", batch_rows, output_factory);
  books_ = OpenBooks(run_dir / "books.parquet", batch_rows, output_factory);
}
ParquetWriter::~ParquetWriter() noexcept = default;

std::unique_ptr<ParquetWriter::Sink> ParquetWriter::OpenSink(
    const std::filesystem::path& path, std::int64_t batch_rows,
    std::shared_ptr<arrow::Schema> schema, const Factory& factory) {
  auto sink = std::make_unique<Sink>();
  sink->file = path.filename().string(); sink->batch_rows = batch_rows;
  sink->schema = std::move(schema);
  sink->builders.reserve(sink->schema->num_fields());
  for (const auto& field : sink->schema->fields()) {
    auto builder = Take(arrow::MakeBuilder(field->type()), sink->file, "MakeBuilder");
    if (!builder->type()->Equals(field->type()))
      throw std::logic_error(sink->file + " / MakeBuilder: 列类型不符");
    sink->builders.emplace_back(std::move(builder));
  }
  sink->output = factory(path);
  if (!sink->output) throw std::runtime_error(sink->file + " / Open: 输出流为空");
  parquet::WriterProperties::Builder properties;
  properties.compression(parquet::Compression::ZSTD);
  parquet::ArrowWriterProperties::Builder arrow_properties;
  arrow_properties.store_schema()->set_use_threads(false);
  sink->writer = Take(parquet::arrow::FileWriter::Open(*sink->schema, arrow::default_memory_pool(),
      sink->output, properties.build(), arrow_properties.build()), sink->file, "FileWriter Open");
  Reserve(*sink);
  return sink;
}

std::unique_ptr<ParquetWriter::Sink> ParquetWriter::OpenOrders(
    const std::filesystem::path& path, std::int64_t rows, const Factory& factory) {
  return OpenSink(path, rows, Schema({I64("seq"), I64("order_id"), I64("price"), I64("qty"),
      I64("new_qty"), I64("event_ns"), I64("recv_ns"), U32("instrument_id"), U8("side"),
      U8("type"), U8("action")}), factory);
}
std::unique_ptr<ParquetWriter::Sink> ParquetWriter::OpenTrades(
    const std::filesystem::path& path, std::int64_t rows, const Factory& factory) {
  return OpenSink(path, rows, Schema({I64("seq"), I64("buy_order_id"), I64("sell_order_id"),
      I64("price"), I64("qty"), I64("event_ns"), I64("recv_ns"), U32("instrument_id"), U8("side")}), factory);
}
std::unique_ptr<ParquetWriter::Sink> ParquetWriter::OpenCancels(
    const std::filesystem::path& path, std::int64_t rows, const Factory& factory) {
  return OpenSink(path, rows, Schema({I64("seq"), I64("order_id"), I64("qty"), I64("event_ns"),
      I64("recv_ns"), U32("instrument_id"), U8("side")}), factory);
}
std::unique_ptr<ParquetWriter::Sink> ParquetWriter::OpenLevels(
    const std::filesystem::path& path, std::int64_t rows, const Factory& factory) {
  return OpenSink(path, rows, Schema({I64("seq"), I64("price"), I64("qty"), I64("event_ns"),
      I64("recv_ns"), U32("instrument_id"), U8("side")}), factory);
}
std::unique_ptr<ParquetWriter::Sink> ParquetWriter::OpenBooks(
    const std::filesystem::path& path, std::int64_t rows, const Factory& factory) {
  return OpenSink(path, rows, Schema({I64("event_ns"), I64("recv_ns"), U32("instrument_id"),
      List("bid_price"), List("bid_qty"), List("ask_price"), List("ask_qty")}), factory);
}

void ParquetWriter::RequireOpen() const {
  if (state_ != State::Open) throw std::logic_error("ParquetWriter: 当前状态禁止追加");
}

#define QBOOK_DEFINE_ADD(type, sink, append) \
  void ParquetWriter::Add(const qlib::type& value) { \
    RequireOpen(); \
    try { append(*sink, value); Commit(*sink); } \
    catch (...) { state_ = State::Failed; throw; } \
  }
QBOOK_DEFINE_ADD(order, orders_, AppendOrder)
QBOOK_DEFINE_ADD(trade, trades_, AppendTrade)
QBOOK_DEFINE_ADD(cancel, cancels_, AppendCancel)
QBOOK_DEFINE_ADD(level, levels_, AppendLevel)
QBOOK_DEFINE_ADD(book, books_, AppendBook)
#undef QBOOK_DEFINE_ADD

void ParquetWriter::AppendOrder(Sink& s, const qlib::order& v) {
  s.I(0).UnsafeAppend(v.seq); s.I(1).UnsafeAppend(v.order_id); s.I(2).UnsafeAppend(v.price);
  s.I(3).UnsafeAppend(v.qty); s.I(4).UnsafeAppend(v.new_qty); s.I(5).UnsafeAppend(v.event_ns);
  s.I(6).UnsafeAppend(v.recv_ns); s.U32(7).UnsafeAppend(v.instrument_id);
  s.U8(8).UnsafeAppend(static_cast<std::uint8_t>(v.side));
  s.U8(9).UnsafeAppend(static_cast<std::uint8_t>(v.type));
  s.U8(10).UnsafeAppend(static_cast<std::uint8_t>(v.action));
}
void ParquetWriter::AppendTrade(Sink& s, const qlib::trade& v) {
  s.I(0).UnsafeAppend(v.seq); s.I(1).UnsafeAppend(v.buy_order_id); s.I(2).UnsafeAppend(v.sell_order_id);
  s.I(3).UnsafeAppend(v.price); s.I(4).UnsafeAppend(v.qty); s.I(5).UnsafeAppend(v.event_ns);
  s.I(6).UnsafeAppend(v.recv_ns); s.U32(7).UnsafeAppend(v.instrument_id);
  s.U8(8).UnsafeAppend(static_cast<std::uint8_t>(v.side));
}
void ParquetWriter::AppendCancel(Sink& s, const qlib::cancel& v) {
  s.I(0).UnsafeAppend(v.seq); s.I(1).UnsafeAppend(v.order_id); s.I(2).UnsafeAppend(v.qty);
  s.I(3).UnsafeAppend(v.event_ns); s.I(4).UnsafeAppend(v.recv_ns);
  s.U32(5).UnsafeAppend(v.instrument_id); s.U8(6).UnsafeAppend(static_cast<std::uint8_t>(v.side));
}
void ParquetWriter::AppendLevel(Sink& s, const qlib::level& v) {
  s.I(0).UnsafeAppend(v.seq); s.I(1).UnsafeAppend(v.price); s.I(2).UnsafeAppend(v.qty);
  s.I(3).UnsafeAppend(v.event_ns); s.I(4).UnsafeAppend(v.recv_ns);
  s.U32(5).UnsafeAppend(v.instrument_id); s.U8(6).UnsafeAppend(static_cast<std::uint8_t>(v.side));
}
void ParquetWriter::AppendBook(Sink& s, const qlib::book& v) {
  s.I(0).UnsafeAppend(v.event_ns); s.I(1).UnsafeAppend(v.recv_ns); s.U32(2).UnsafeAppend(v.instrument_id);
  const std::int64_t* values[] = {v.bid_price, v.bid_qty, v.ask_price, v.ask_qty};
  for (std::size_t i = 0; i < 4; ++i) {
    auto& list = static_cast<arrow::FixedSizeListBuilder&>(*s.builders[i + 3]);
    Check(list.Append(), s.file, "AppendBook list");
    auto& child = static_cast<arrow::Int64Builder&>(*list.value_builder());
    for (std::size_t depth = 0; depth < qlib::snapshot_depth; ++depth)
      child.UnsafeAppend(values[i][depth]);
  }
}

void ParquetWriter::Reserve(Sink& sink) {
  CheckCapacity(sink.batch_rows);
  for (const auto& builder : sink.builders) {
    Check(builder->Reserve(sink.batch_rows), sink.file, "Reserve");
    if (builder->type()->id() == arrow::Type::FIXED_SIZE_LIST) {
      auto& list = static_cast<arrow::FixedSizeListBuilder&>(*builder);
      Check(list.value_builder()->Reserve(sink.batch_rows * qlib::snapshot_depth), sink.file, "Reserve child");
    }
  }
}
void ParquetWriter::Commit(Sink& sink) {
  if (sink.rows >= sink.batch_rows) throw std::logic_error(sink.file + " / Commit: 批次已满");
  if (++sink.rows == sink.batch_rows) Flush(sink);
}
void ParquetWriter::Flush(Sink& sink) {
  if (sink.rows == 0) return;
  const auto rows = sink.rows;
  std::vector<std::shared_ptr<arrow::Array>> arrays;
  arrays.reserve(sink.builders.size());
  for (const auto& builder : sink.builders)
    arrays.push_back(Take(builder->Finish(), sink.file, "Finish"));
  auto batch = arrow::RecordBatch::Make(sink.schema, rows, std::move(arrays));
  Check(batch->ValidateFull(), sink.file, "RecordBatch validation");
  auto table = Take(arrow::Table::FromRecordBatches({batch}), sink.file, "Table creation");
  Check(sink.writer->WriteTable(*table, rows), sink.file, "WriteTable");
  sink.rows = 0;
  Reserve(sink);
}
void ParquetWriter::Cleanup() noexcept {
  for (auto* sink : {orders_.get(), trades_.get(), cancels_.get(), levels_.get(), books_.get()})
    if (sink) sink->Cleanup();
}
void ParquetWriter::Close() {
  if (state_ == State::Closed) return;
  if (state_ == State::Failed) {
    Cleanup();
    throw std::logic_error("ParquetWriter: Failed 状态不能正常关闭");
  }
  try {
    for (auto* sink : {orders_.get(), trades_.get(), cancels_.get(), levels_.get(), books_.get()}) {
      Flush(*sink);
      Check(sink->writer->Close(), sink->file, "FileWriter Close");
      if (!sink->output->closed()) Check(sink->output->Close(), sink->file, "OutputStream Close");
      sink->writer.reset(); sink->output.reset();
    }
    state_ = State::Closed;
  } catch (...) {
    const auto error = std::current_exception();
    state_ = State::Failed;
    Cleanup();
    std::rethrow_exception(error);
  }
}
}  // namespace qbook
