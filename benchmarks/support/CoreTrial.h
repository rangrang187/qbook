#pragma once
#include "BenchmarkCase.h"
namespace qbook::bench {
const std::vector<std::string>& CoreOperations();
PreparedBenchmark PrepareCoreBenchmark(const CaseSpec&, const ExperimentSpec&);
}  // namespace qbook::bench
