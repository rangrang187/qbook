#include "support/PipelineTrial.h"

#include <string_view>

int main(int argc, char** argv) {
  if (argc > 1 && std::string_view(argv[1]) == "--worker")
    return qbook::bench::PipelineWorkerMain(argc, argv);
  return qbook::bench::BenchmarkMain(argc, argv, "pipeline/", qbook::bench::PipelineOperations(),
                                    qbook::bench::PreparePipelineBenchmark);
}
