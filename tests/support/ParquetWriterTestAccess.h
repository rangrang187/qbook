#pragma once

#include "ParquetWriter.h"
#include "detail/OutputStreams.h"

namespace qbook::testing {

struct ParquetWriterTestAccess {
  static std::unique_ptr<ParquetWriter> Create(const std::filesystem::path& directory,
                                              std::int64_t batch_rows,
                                              const detail::OutputStreamFactory& factory) {
    return std::unique_ptr<ParquetWriter>(new ParquetWriter(directory, batch_rows, factory));
  }
  static bool Failed(const ParquetWriter& writer) {
    return writer.state_ == ParquetWriter::State::Failed;
  }
  static bool Closed(const ParquetWriter& writer) {
    return writer.state_ == ParquetWriter::State::Closed;
  }
};

}  // namespace qbook::testing
