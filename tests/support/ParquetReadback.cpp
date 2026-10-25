#include "ParquetReadback.h"

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>
#include <parquet/file_reader.h>

#include <array>
#include <stdexcept>
#include <string>

namespace qbook::testing {
namespace {
void Check(const arrow::Status& status) {
  if (!status.ok()) throw std::runtime_error(status.ToString());
}
template <class T> T Take(arrow::Result<T> result) {
  Check(result.status());
  return std::move(result).ValueOrDie();
}
void Expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error("Parquet readback: " + message);
}

using Fields = std::vector<std::shared_ptr<arrow::Field>>;
std::shared_ptr<arrow::Field> I64(const char* name) { return arrow::field(name, arrow::int64(), false); }
std::shared_ptr<arrow::Field> U32(const char* name) { return arrow::field(name, arrow::uint32(), false); }
std::shared_ptr<arrow::Field> U8(const char* name) { return arrow::field(name, arrow::uint8(), false); }
std::shared_ptr<arrow::Field> List(const char* name) {
  return arrow::field(name, arrow::fixed_size_list(arrow::field("item", arrow::int64(), false), 1), false);
}

std::shared_ptr<arrow::Table> Read(const std::filesystem::path& path,
                                  std::int64_t max_rows, const Fields& fields) {
  auto input = Take(arrow::io::ReadableFile::Open(path.string()));
  auto reader = Take(parquet::arrow::OpenFile(input, arrow::default_memory_pool()));
  reader->set_use_threads(false);
  const auto metadata = reader->parquet_reader()->metadata();
  Expect(metadata->num_rows() >= 0 && metadata->num_rows() <= max_rows, "行数超过测试上限");
  Expect(metadata->key_value_metadata() && metadata->key_value_metadata()->Contains("ARROW:schema"),
         "缺少 ARROW:schema");
  Expect(metadata->num_columns() == static_cast<int>(fields.size()), "列数量不符");
  for (int column = 0; column < metadata->num_columns(); ++column) {
    const auto* leaf = metadata->schema()->Column(column);
    const auto type = fields[column]->type()->id();
    const bool unsigned_integer = type == arrow::Type::UINT32 || type == arrow::Type::UINT8;
    Expect(leaf->physical_type() == (unsigned_integer ? parquet::Type::INT32 : parquet::Type::INT64),
           "Parquet 物理整数类型不符");
    if (unsigned_integer) {
      Expect(leaf->logical_type()->is_int(), "Parquet 缺少无符号整数注解");
      const auto& logical = static_cast<const parquet::IntLogicalType&>(*leaf->logical_type());
      Expect(!logical.is_signed() && logical.bit_width() == (type == arrow::Type::UINT32 ? 32 : 8),
             "Parquet 无符号整数位宽不符");
    }
  }
  for (int group = 0; group < metadata->num_row_groups(); ++group) {
    const auto row_group = metadata->RowGroup(group);
    for (int column = 0; column < row_group->num_columns(); ++column)
      Expect(row_group->ColumnChunk(column)->compression() == parquet::Compression::ZSTD,
             "列块未使用 ZSTD");
  }
  auto table = Take(reader->ReadTable());
  Check(table->ValidateFull());
  Expect(table->num_columns() == static_cast<int>(fields.size()), "列数量不符");
  Expect(table->num_rows() == metadata->num_rows(), "footer 与读取行数不符");
  const std::array<std::pair<const char*, const char*>, 8> expected_metadata{{
      {"qbook.format_version", "1"}, {"qbook.price_scale", "10000000000"},
      {"qbook.qty_scale", "100000000"}, {"qbook.time_unit", "unix_epoch_ns"},
      {"qbook.snapshot_depth", "1"}, {"qbook.side", "0:buy,1:sell"},
      {"qbook.order_type", "0:limit,1:market"}, {"qbook.order_action", "0:add,1:modify"}}};
  Expect(table->schema()->metadata() != nullptr, "缺少格式元数据");
  for (const auto& [key, value] : expected_metadata)
    Expect(Take(table->schema()->metadata()->Get(key)) == value, std::string("元数据不符: ") + key);
  for (int column = 0; column < table->num_columns(); ++column) {
    const auto& expected = fields[column];
    const auto actual = table->field(column);
    Expect(actual->name() == expected->name() && !actual->nullable(), "列名、顺序或非空约束不符");
    Expect(actual->type()->id() == expected->type()->id(), "列逻辑类型不符: " + actual->name());
    if (actual->type()->id() == arrow::Type::FIXED_SIZE_LIST) {
      const auto& type = static_cast<const arrow::FixedSizeListType&>(*actual->type());
      Expect(type.list_size() == 1 && type.value_type()->id() == arrow::Type::INT64 &&
             !type.value_field()->nullable(), "快照列表类型、深度或子值非空约束不符");
    } else {
      Expect(actual->type()->Equals(expected->type()), "标量类型不符");
    }
    for (const auto& chunk : table->column(column)->chunks()) {
      Expect(chunk->null_count() == 0, "列包含 null");
      if (chunk->type_id() == arrow::Type::FIXED_SIZE_LIST)
        Expect(static_cast<const arrow::FixedSizeListArray&>(*chunk).values()->null_count() == 0,
               "快照子值包含 null");
    }
  }
  Check(input->Close());
  return table;
}

// 每列独立定位 chunk，允许各列使用不同分块边界。
std::pair<const arrow::Array*, std::int64_t> Cell(const arrow::Table& table, int column,
                                                std::int64_t row) {
  for (const auto& chunk : table.column(column)->chunks()) {
    if (row < chunk->length()) return {chunk.get(), row};
    row -= chunk->length();
  }
  throw std::runtime_error("Parquet readback: 行索引超界");
}
std::int64_t Integer(const arrow::Table& table, int column, std::int64_t row) {
  const auto [array, offset] = Cell(table, column, row);
  switch (array->type_id()) {
    case arrow::Type::INT64: return static_cast<const arrow::Int64Array*>(array)->Value(offset);
    case arrow::Type::UINT32: return static_cast<const arrow::UInt32Array*>(array)->Value(offset);
    case arrow::Type::UINT8: return static_cast<const arrow::UInt8Array*>(array)->Value(offset);
    case arrow::Type::FIXED_SIZE_LIST: {
      const auto& list = static_cast<const arrow::FixedSizeListArray&>(*array);
      return static_cast<const arrow::Int64Array&>(*list.values()).Value(list.value_offset(offset));
    }
    default: throw std::runtime_error("Parquet readback: 非整数列");
  }
}
template <std::size_t N>
void Row(const arrow::Table& table, std::int64_t row, const std::array<std::int64_t, N>& values) {
  for (std::size_t column = 0; column < N; ++column)
    Expect(Integer(table, static_cast<int>(column), row) == values[column],
           "行 " + std::to_string(row) + " 列 " + table.field(static_cast<int>(column))->name() + " 值不符");
}
template <class T> void Count(const arrow::Table& table, const std::vector<T>& rows) {
  Expect(table.num_rows() == static_cast<std::int64_t>(rows.size()), "预期行数不符");
}
}

ReadbackResult ReadRunDirectory(const std::filesystem::path& directory, std::int64_t max_rows_per_file) {
  if (max_rows_per_file < 0) throw std::invalid_argument("读取行数上限应为非负数");
  ReadbackResult result;
  result.orders = Read(directory / "orders.parquet", max_rows_per_file,
      {I64("seq"), I64("order_id"), I64("price"), I64("qty"), I64("new_qty"), I64("event_ns"),
       I64("recv_ns"), U32("instrument_id"), U8("side"), U8("type"), U8("action")});
  result.trades = Read(directory / "trades.parquet", max_rows_per_file,
      {I64("seq"), I64("buy_order_id"), I64("sell_order_id"), I64("price"), I64("qty"),
       I64("event_ns"), I64("recv_ns"), U32("instrument_id"), U8("side")});
  result.cancels = Read(directory / "cancels.parquet", max_rows_per_file,
      {I64("seq"), I64("order_id"), I64("qty"), I64("event_ns"), I64("recv_ns"),
       U32("instrument_id"), U8("side")});
  result.levels = Read(directory / "levels.parquet", max_rows_per_file,
      {I64("seq"), I64("price"), I64("qty"), I64("event_ns"), I64("recv_ns"),
       U32("instrument_id"), U8("side")});
  result.books = Read(directory / "books.parquet", max_rows_per_file,
      {I64("event_ns"), I64("recv_ns"), U32("instrument_id"), List("bid_price"), List("bid_qty"),
       List("ask_price"), List("ask_qty")});
  return result;
}

void CheckExpectedFiles(const ReadbackResult& actual, const ExpectedRun& expected) {
  Count(*actual.orders, expected.orders); Count(*actual.trades, expected.trades);
  Count(*actual.cancels, expected.cancels); Count(*actual.levels, expected.levels);
  Count(*actual.books, expected.books);
  for (std::size_t i = 0; i < expected.orders.size(); ++i) {
    const auto& v = expected.orders[i];
    Row(*actual.orders, i, std::array<std::int64_t, 11>{v.seq,v.order_id,v.price,v.qty,v.new_qty,
        v.event_ns,v.recv_ns,v.instrument_id,static_cast<std::uint8_t>(v.side),
        static_cast<std::uint8_t>(v.type),static_cast<std::uint8_t>(v.action)});
  }
  for (std::size_t i = 0; i < expected.trades.size(); ++i) {
    const auto& v = expected.trades[i];
    Row(*actual.trades, i, std::array<std::int64_t, 9>{v.seq,v.buy_order_id,v.sell_order_id,v.price,
        v.qty,v.event_ns,v.recv_ns,v.instrument_id,static_cast<std::uint8_t>(v.side)});
  }
  for (std::size_t i = 0; i < expected.cancels.size(); ++i) {
    const auto& v = expected.cancels[i];
    Row(*actual.cancels, i, std::array<std::int64_t, 7>{v.seq,v.order_id,v.qty,v.event_ns,v.recv_ns,
        v.instrument_id,static_cast<std::uint8_t>(v.side)});
  }
  for (std::size_t i = 0; i < expected.levels.size(); ++i) {
    const auto& v = expected.levels[i];
    Row(*actual.levels, i, std::array<std::int64_t, 7>{v.seq,v.price,v.qty,v.event_ns,v.recv_ns,
        v.instrument_id,static_cast<std::uint8_t>(v.side)});
  }
  for (std::size_t i = 0; i < expected.books.size(); ++i) {
    const auto& v = expected.books[i];
    Row(*actual.books, i, std::array<std::int64_t, 7>{v.event_ns,v.recv_ns,v.instrument_id,
        v.bid_price[0],v.bid_qty[0],v.ask_price[0],v.ask_qty[0]});
  }
}
}  // namespace qbook::testing
