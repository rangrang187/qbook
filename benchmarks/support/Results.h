#pragma once

#include "BenchmarkCase.h"
#include <ostream>

namespace qbook::bench {
inline constexpr std::string_view kCsvHeader =
    "schema_version,run_id,case_id,trial_index,measurement_kind,phase,count_kind,work_count,duration_ns,sample_mode,checksum,validation_status,code_id,build_id,workload_hash,validation_message";
struct MeasurementResult {
  std::string run_id, case_id;
  std::uint64_t trial_index = 0;
  std::string measurement_kind;
  PhaseMeasurement measurement;
  std::string sample_mode = "batch-average";
  std::string validation_status = "pending";
  std::string code_id, build_id, workload_hash, validation_message;
};
void WriteResult(const MeasurementResult& result, std::ostream& csv);
}  // namespace qbook::bench
