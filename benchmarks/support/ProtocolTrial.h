#pragma once
#include "BenchmarkCase.h"
namespace qbook::bench {
const std::vector<std::string>& ProtocolOperations();
PreparedBenchmark PrepareProtocolBenchmark(const CaseSpec&, const ExperimentSpec&);
}  // namespace qbook::bench
