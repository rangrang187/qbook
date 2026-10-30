#include "Results.h"
#include "Measure.h"
#include "Workloads.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/statfs.h>
#include <sys/utsname.h>
#include <sched.h>
#include <unistd.h>

#ifndef QBOOK_SOURCE_DIR
#define QBOOK_SOURCE_DIR "."
#endif
#ifndef QBOOK_BUILD_DIR
#define QBOOK_BUILD_DIR "."
#endif

namespace qbook::bench {
namespace {
std::string CsvField(std::string_view value) {
  if (value.find_first_of(",\r\n\"") == std::string_view::npos) return std::string(value);
  std::string result = "\"";
  for (char c : value) { result += c; if (c == '"') result += '"'; }
  return result + '"';
}
std::string ReadText(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("cannot read evidence: " + path.string());
  std::string result((std::istreambuf_iterator<char>(stream)), {});
  if (stream.bad()) throw std::runtime_error("cannot finish evidence read: " + path.string());
  return result;
}
std::string QuoteShell(std::string_view text) {
  std::string result = "'";
  for (char c : text) result += c == '\'' ? "'\\''" : std::string(1, c);
  return result + '\'';
}
std::string Git(std::string_view fixed_arguments) {
  const auto command = "git -C " + QuoteShell(QBOOK_SOURCE_DIR) + ' ' + std::string(fixed_arguments);
  FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) throw std::runtime_error("cannot start read-only Git identity query");
  std::string result; char data[8192];
  for (;;) {
    const auto n = fread(data, 1, sizeof(data), pipe);
    result.append(data, n);
    if (n != sizeof(data)) break;
    if (result.size() > 32 * 1024 * 1024) { pclose(pipe); throw std::runtime_error("Git identity output exceeds budget"); }
  }
  const bool failed = ferror(pipe);
  const int status = pclose(pipe);
  if (failed || status != 0) throw std::runtime_error("Git identity query failed");
  return result;
}
std::string Trim(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
  return value;
}
std::string HashText(std::string_view value) { WorkloadHasher h; h.String(value); return h.Finish(); }
std::map<std::string, std::string> Cache() {
  std::map<std::string, std::string> result;
  std::istringstream input(ReadText(std::filesystem::path(QBOOK_BUILD_DIR) / "CMakeCache.txt"));
  for (std::string line; std::getline(input, line);) {
    if (line.starts_with("//") || line.starts_with('#')) continue;
    const auto colon = line.find(':'); const auto equal = line.find('=');
    if (colon != std::string::npos && equal != std::string::npos && colon < equal)
      result.emplace(line.substr(0, colon), line.substr(equal + 1));
  }
  return result;
}
std::string SourceIdentity() {
  const auto names = Git("ls-files --cached --others --exclude-standard -z");
  std::set<std::string> paths;
  std::size_t offset = 0;
  while (offset < names.size()) {
    const auto end = names.find('\0', offset);
    if (end == std::string::npos) throw std::runtime_error("invalid Git file list");
    const auto name = names.substr(offset, end - offset); offset = end + 1;
    // 性能结果和说明文档不是被测代码；避免输出自身改变下一次源码身份。
    const bool source = name.starts_with("include/") || name.starts_with("src/") ||
      name.starts_with("benchmarks/") || name.starts_with("tests/") || name.starts_with("cmake/") ||
      name.starts_with("config/") || name == "CMakeLists.txt" || name.ends_with(".sha256") ||
      name == "ui/package-lock.json";
    if (source && !name.ends_with(".md") && !name.ends_with(".pyc")) paths.insert(name);
  }
  WorkloadHasher hash;
  for (const auto& name : paths) {
    const auto path = std::filesystem::path(QBOOK_SOURCE_DIR) / name;
    hash.String(name);
    hash.String(std::filesystem::is_regular_file(path) ? HashFile(path) : "missing");
  }
  return hash.Finish();
}
std::string Environment(const std::filesystem::path& output) {
  utsname u{}; if (uname(&u) != 0) throw std::runtime_error("uname failed");
  std::string cpu = "unknown", memory = "unknown";
  std::istringstream cpuinfo(ReadText("/proc/cpuinfo"));
  for (std::string line; std::getline(cpuinfo, line);) if (line.starts_with("model name")) { cpu = line.substr(line.find(':') + 2); break; }
  std::istringstream meminfo(ReadText("/proc/meminfo"));
  for (std::string key; meminfo >> key;) { if (key == "MemTotal:") { meminfo >> memory; break; } std::string rest; std::getline(meminfo, rest); }
  cpu_set_t set; CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) != 0) throw std::runtime_error("sched_getaffinity failed");
  std::ostringstream affinity; bool first = true;
  for (int i = 0; i < CPU_SETSIZE; ++i) if (CPU_ISSET(i, &set)) { if (!first) affinity << ','; affinity << i; first = false; }
  struct statfs fs{}; if (statfs(output.c_str(), &fs) != 0) throw std::runtime_error("statfs failed");
  std::ostringstream filesystem; filesystem << "linux-type:0x" << std::hex << fs.f_type;
  std::ostringstream out;
  out << "{\"os\":" << JsonString(u.sysname) << ",\"kernel\":" << JsonString(u.release)
      << ",\"machine\":" << JsonString(u.machine) << ",\"cpu_model\":" << JsonString(cpu)
      << ",\"logical_cpus\":" << sysconf(_SC_NPROCESSORS_ONLN)
      << ",\"memory_total_kb\":" << JsonString(memory) << ",\"affinity\":" << JsonString(affinity.str())
      << ",\"filesystem\":" << JsonString(filesystem.str())
      << ",\"frequency_control\":\"uncontrolled\",\"background_load\":\"uncontrolled\"}";
  return out.str();
}
void WriteFd(int fd, std::string_view data) {
  while (!data.empty()) {
    const auto n = write(fd, data.data(), data.size());
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw std::runtime_error("benchmark evidence write failed: " + std::string(std::strerror(errno)));
    data.remove_prefix(static_cast<std::size_t>(n));
  }
}
void ExclusiveWrite(const std::filesystem::path& file, std::string_view data) {
  const int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) throw std::runtime_error("exclusive evidence create failed: " + file.string());
  try { WriteFd(fd, data); } catch (...) { close(fd); throw; }
  if (close(fd) != 0) throw std::runtime_error("evidence close failed: " + file.string());
}
std::string TrialJson(const PreparedBenchmark& prepared, const TrialContext& context, const TrialResult& result) {
  if (result.details_json.empty() || result.details_json.front() != '{' || result.details_json.back() != '}')
    throw std::invalid_argument("trial details must be a fixed JSON object");
  std::ostringstream out;
  out << "{\"case_id\":" << JsonString(prepared.spec.case_id) << ",\"trial_index\":" << context.trial_index
      << ",\"validation_status\":" << JsonString(result.validation_status)
      << ",\"validation_message\":" << JsonString(result.validation_message)
      << ",\"details\":" << result.details_json << '}';
  return out.str();
}
std::string JoinJson(const std::vector<std::string>& entries) {
  std::string result = "[";
  for (std::size_t i = 0; i < entries.size(); ++i) { if (i) result += ','; result += entries[i]; }
  return result + ']';
}
}  // namespace

void WriteResult(const MeasurementResult& result, std::ostream& csv) {
  ValidateMeasurement(result.measurement, result.validation_status == "valid");
  const auto& m = result.measurement;
  csv << kSchemaVersion << ',' << CsvField(result.run_id) << ',' << CsvField(result.case_id) << ','
      << result.trial_index << ',' << CsvField(result.measurement_kind) << ',' << CsvField(m.phase) << ','
      << CsvField(m.count_kind) << ',' << m.work_count << ',' << m.duration_ns << ',' << CsvField(result.sample_mode)
      << ',' << m.checksum << ',' << CsvField(result.validation_status) << ',' << CsvField(result.code_id) << ','
      << CsvField(result.build_id) << ',' << CsvField(result.workload_hash) << ',' << CsvField(result.validation_message) << '\n';
  if (!csv) throw std::runtime_error("measurement CSV write failed");
}

int BenchmarkMain(int argc, char** argv, std::string_view family,
                  const std::vector<std::string>& operations, const PrepareFunction& factory) {
  std::filesystem::path output;
  bool owns_output = false;
  std::function<void(bool, const std::string&)> manifest;
  int csv_fd = -1;
  try {
    const auto options = ParseOptions(argc, argv);
    auto experiment = LoadExperiment(options.config);
    std::vector<CaseSpec> selected;
    for (auto s : experiment.cases) {
      if (!s.operation.starts_with(family)) continue;
      if (std::find(operations.begin(), operations.end(), s.operation) == operations.end())
        throw std::invalid_argument("unknown operation: " + s.operation);
      if (options.selected_case == "all" || options.selected_case == s.case_id) selected.push_back(std::move(s));
    }
    if (selected.empty()) throw std::invalid_argument("no configured cases match this executable and --case");
    if (options.list_cases) {
      for (const auto& s : selected) std::cout << s.case_id << '\n';
      return 0;
    }
    if (options.smoke) {
      experiment.measurement_kind = "smoke"; experiment.repetitions = 1; experiment.warmup_rounds = 0;
      for (auto& s : selected) s = SmokeCase(s);
    }
    for (const auto& s : selected) ValidateCase(s, experiment);
    const auto cache = Cache();
    auto cached = [&](const std::string& key) { const auto it = cache.find(key); return it == cache.end() ? std::string{} : it->second; };
    if (experiment.measurement_kind != "smoke" && cached("CMAKE_BUILD_TYPE") != "Release")
      throw std::invalid_argument("formal benchmarks require an actual Release build");
    const auto ipo = cached("CMAKE_INTERPROCEDURAL_OPTIMIZATION");
    if (experiment.measurement_kind != "smoke" && ipo != "OFF")
      throw std::invalid_argument("formal V5 baseline requires CMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF");
    output = std::filesystem::absolute(options.output);
    if (!std::filesystem::create_directory(output)) throw std::runtime_error("output directory already exists: " + output.string());
    owns_output = true;
    if (std::filesystem::space(output).available < experiment.max_output_bytes)
      throw std::runtime_error("available storage is below the frozen per-run output budget");
    const auto executable = std::filesystem::read_symlink("/proc/self/exe");
    const auto code_id = SourceIdentity();
    const auto binary_hash = HashFile(executable);
    WorkloadHasher build_hash; build_hash.String(binary_hash);
    build_hash.String(HashFile(std::filesystem::path(QBOOK_BUILD_DIR) / "CMakeCache.txt"));
    build_hash.String(HashFile(std::filesystem::path(QBOOK_BUILD_DIR) / "compile_commands.json"));
    const auto build_id = build_hash.Finish();
    const auto run_id = output.filename().string() + "-" + std::to_string(getpid());
    const auto status = Git("status --porcelain");
    const auto head = Trim(Git("rev-parse HEAD"));
    const auto patch_hash = HashText(Git("diff --binary HEAD"));
    const auto config_hash = HashFile(options.config);
    const auto environment = Environment(output);
    std::vector<std::string> command;
    for (int i = 0; i < argc; ++i) command.push_back(JsonString(argv[i]));
    std::vector<PreparedBenchmark> prepared;
    std::vector<std::string> cases, trials, warmups;
    std::size_t manifest_revision = 0;
    std::ostringstream policy;
    policy << "{\"build_type\":" << JsonString(cached("CMAKE_BUILD_TYPE")) << ",\"ipo\":" << JsonString(ipo)
           << ",\"compiler_id\":\"GNU-compatible\",\"compiler_version\":" << JsonString(__VERSION__)
           << ",\"cxx_flags\":" << JsonString(cached("CMAKE_CXX_FLAGS"))
           << ",\"cxx_flags_release\":" << JsonString(cached("CMAKE_CXX_FLAGS_RELEASE"))
           << ",\"dependency_manifests\":{";
    bool first_dependency = true;
    for (const auto name : {"qlib", "Catch2", "fkYAML", "spdlog", "libzmq", "cppzmq"}) {
      if (!first_dependency) policy << ',';
      first_dependency = false;
      policy << JsonString(name) << ':' << JsonString(HashFile(std::filesystem::path(QBOOK_SOURCE_DIR) / "third_party" / (std::string(name)+".sha256")));
    }
    policy << "},\"arrow_package_config\":";
    const auto arrow_config = std::filesystem::path(cached("Arrow_DIR")) / "ArrowConfigVersion.cmake";
    const auto parquet_config = std::filesystem::path(cached("Parquet_DIR")) / "ParquetConfigVersion.cmake";
    policy << JsonString(cached("Arrow_DIR").empty() ? "not-linked" : HashFile(arrow_config))
           << ",\"parquet_package_config\":" << JsonString(cached("Parquet_DIR").empty() ? "not-linked" : HashFile(parquet_config)) << '}';
    manifest = [&](bool complete, const std::string& error) {
      std::ostringstream out;
      out << "{\n\"schema_version\":1,\"run_id\":" << JsonString(run_id)
          << ",\"measurement_kind\":" << JsonString(experiment.measurement_kind)
          << ",\"complete\":" << (complete ? "true" : "false") << ",\"command\":" << JoinJson(command)
          << ",\"code\":{\"id\":" << JsonString(code_id) << ",\"git_head\":" << JsonString(head)
          << ",\"dirty\":" << (status.empty() ? "false" : "true") << ",\"source_root\":" << JsonString(QBOOK_SOURCE_DIR)
          << ",\"content_hash\":" << JsonString(code_id) << ",\"patch_hash\":" << JsonString(patch_hash)
          << ",\"content_scope\":\"source,tests,benchmark parameters,build config,dependency manifests,UI lock; no reports or generated outputs\"}"
          << ",\"build\":{\"id\":" << JsonString(build_id) << ",\"executable\":" << JsonString(executable.string())
          << ",\"binary_hash\":" << JsonString(binary_hash) << ",\"build_directory\":" << JsonString(QBOOK_BUILD_DIR)
          << ",\"policy\":" << policy.str() << "},\"environment\":" << environment
          << ",\"experiment_id\":" << JsonString(experiment.experiment_id)
          << ",\"config_hash\":" << JsonString(config_hash)
          << ",\"generator_version\":" << JsonString(kGeneratorVersion) << ",\"seed\":" << experiment.seed
          << ",\"warmup_rounds\":" << experiment.warmup_rounds << ",\"repetitions\":" << experiment.repetitions
          << ",\"max_input_bytes\":" << experiment.max_input_bytes << ",\"max_output_bytes\":" << experiment.max_output_bytes
          << ",\"csv_files\":[\"measurements.csv\"],\"cases\":" << JoinJson(cases)
          << ",\"trials\":" << JoinJson(trials) << ",\"warmup_trials\":" << JoinJson(warmups)
          << ",\"error\":" << JsonString(error) << "\n}\n";
      const auto temp = output / ("manifest.tmp." + std::to_string(manifest_revision++));
      ExclusiveWrite(temp, out.str());
      std::filesystem::rename(temp, output / "manifest.json");
    };
    manifest(false, "");
    csv_fd = open((output / "measurements.csv").c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (csv_fd < 0) throw std::runtime_error("cannot exclusively create measurement CSV");
    WriteFd(csv_fd, std::string(kCsvHeader) + '\n');
    for (const auto& s : selected) {
      auto p = factory(s, experiment);
      if (p.spec.case_id != s.case_id || !p.run || p.phases.empty() || !p.workload_hash.starts_with("fnv1a64:"))
        throw std::logic_error("invalid prepared benchmark registration");
      std::vector<std::string> phases;
      std::set<std::string> names;
      for (const auto& phase : p.phases) {
        if (!names.insert(phase.phase).second) throw std::logic_error("duplicate registered phase");
        phases.push_back("{\"phase\":" + JsonString(phase.phase) + ",\"count_kind\":" + JsonString(phase.count_kind) + ",\"timing_boundary\":" + JsonString(phase.timing_boundary) + '}');
      }
      cases.push_back("{\"case_id\":" + JsonString(s.case_id) + ",\"workload_hash\":" + JsonString(p.workload_hash) + ",\"parameters\":" + CanonicalCaseJson(p.spec) + ",\"expected_phases\":" + JoinJson(phases) + ",\"instrumentation\":" + JsonString(p.instrumentation) + '}');
      prepared.push_back(std::move(p));
    }
    manifest(false, "");
    bool invalid = false;
    for (std::size_t index = 0; index < prepared.size(); ++index) {
      const auto& p = prepared[index];
      for (std::uint64_t round = 0; round < experiment.warmup_rounds + experiment.repetitions; ++round) {
        const bool warmup = round < experiment.warmup_rounds;
        TrialContext context{output / ("work-case-" + std::to_string(index) + (warmup ? "-warmup-" : "-trial-") + std::to_string(round)), warmup,
          warmup ? round : round - experiment.warmup_rounds, experiment.measurement_kind};
        if (!std::filesystem::create_directory(context.work_dir)) throw std::runtime_error("trial work directory collision");
        auto result = p.run(context);
        if (result.validation_status != "valid" && result.validation_status != "invalid") throw std::logic_error("trial must complete validation");
        if (result.measurements.size() != p.phases.size()) throw std::logic_error("trial phase count differs from registered phases");
        std::set<std::string> phases;
        for (const auto& measurement : result.measurements) {
          const auto found = std::find_if(p.phases.begin(), p.phases.end(), [&](const auto& phase) { return phase.phase == measurement.phase && phase.count_kind == measurement.count_kind; });
          if (found == p.phases.end() || !phases.insert(measurement.phase).second) throw std::logic_error("trial phase/count_kind mismatch");
          ValidateMeasurement(measurement, result.validation_status == "valid");
          if (!warmup) {
            auto message = result.validation_message;
            if (result.validation_status == "valid" && measurement.count_kind == "none" && measurement.duration_ns == 0)
              message += (message.empty() ? "" : "; ") + std::string("below clock resolution");
            std::ostringstream row;
            WriteResult({run_id, p.spec.case_id, context.trial_index, experiment.measurement_kind, measurement,
                "batch-average", result.validation_status, code_id, build_id, p.workload_hash, message}, row);
            WriteFd(csv_fd, row.str());
          }
        }
        (warmup ? warmups : trials).push_back(TrialJson(p, context, result));
        invalid = invalid || result.validation_status != "valid";
        manifest(false, "");
        std::cout << p.spec.case_id << (warmup ? " warmup " : " trial ") << context.trial_index << ' ' << result.validation_status << '\n';
        // 空工作目录可以删除；模块生成的文件须由模块成功读回后自行处理。
        if (std::filesystem::is_empty(context.work_dir)) std::filesystem::remove(context.work_dir);
      }
    }
    if (close(csv_fd) != 0) { csv_fd = -1; throw std::runtime_error("measurement CSV close failed"); }
    csv_fd = -1;
    manifest(true, invalid ? "one or more trials failed validation" : "");
    return invalid ? 1 : 0;
  } catch (const std::exception& error) {
    // manifest 回调借用 try 内状态，异常展开后不能再调用；保留上次 complete=false 清单。
    if (csv_fd >= 0) close(csv_fd);
    if (owns_output) {
      try {
        ExclusiveWrite(output / "failure.txt", std::string(error.what()) + '\n');
        const auto target = output / "manifest.json";
        std::string failed;
        if (std::filesystem::exists(target)) {
          failed = ReadText(target);
          const auto marker = failed.rfind("\"error\":\"\"");
          if (marker != std::string::npos) failed.replace(marker, 10, "\"error\":" + JsonString(error.what()));
        } else {
          failed = "{\"schema_version\":1,\"complete\":false,\"error\":" + JsonString(error.what()) + "}\n";
        }
        ExclusiveWrite(output / "manifest.failure.tmp", failed);
        std::filesystem::rename(output / "manifest.failure.tmp", target);
      } catch (...) {}
    }
    std::cerr << "benchmark failed: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace qbook::bench
