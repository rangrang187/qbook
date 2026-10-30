#pragma once

#include "BenchmarkCase.h"
#include "detail/OutputStreams.h"

namespace qbook::bench {

// 故障工厂只供实验正确性检查；正常基线直接使用公开 ParquetWriter 构造函数。
PreparedBenchmark PrepareWriterBenchmark(const CaseSpec& spec, const ExperimentSpec& experiment,
    detail::OutputStreamFactory output_factory = {});

}  // namespace qbook::bench
