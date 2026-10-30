#include "support/CoreTrial.h"
int main(int argc, char** argv) {
  return qbook::bench::BenchmarkMain(argc, argv, "core.", qbook::bench::CoreOperations(),
                                    qbook::bench::PrepareCoreBenchmark);
}
