#include "support/ProtocolTrial.h"
int main(int argc, char** argv) {
  return qbook::bench::BenchmarkMain(argc, argv, "protocol.", qbook::bench::ProtocolOperations(),
                                    qbook::bench::PrepareProtocolBenchmark);
}
