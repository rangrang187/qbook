#include "support/WriterTrial.h"

int main(int argc, char** argv) {
  return qbook::bench::BenchmarkMain(argc,argv,"writer",
      {"writer.empty","writer.orders","writer.trades","writer.cancels","writer.levels",
       "writer.books","writer.mixed"},
      [](const auto& spec,const auto& experiment){
        return qbook::bench::PrepareWriterBenchmark(spec,experiment);
      });
}
