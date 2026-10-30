#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <sys/types.h>

namespace qbook::testing {

struct ProcessResult {
  int exit_code = -1;
  int term_signal = 0;
  int exec_error = 0;
  bool timed_out = false;
  bool output_overflow = false;
  std::string output;
};

// 在独占工作目录执行一个子进程；输出有上限，超时清理只作用于本次 PID。
class ProcessHarness {
public:
  using Deadline = std::chrono::steady_clock::time_point;
  explicit ProcessHarness(std::filesystem::path executable,
                          std::vector<std::string> arguments = {});
  ~ProcessHarness() noexcept;
  ProcessHarness(const ProcessHarness&) = delete;
  ProcessHarness& operator=(const ProcessHarness&) = delete;

  const std::filesystem::path& Directory() const noexcept { return directory_; }
  void WriteConfig(std::string_view yaml);
  void Start();
  // 非阻塞回收输出与退出状态，供有限速率发布者在调度时轮询。
  void Poll();
  void WaitForOutput(std::string_view marker, Deadline deadline);
  void SendSignal(int signal);
  const ProcessResult& Wait(Deadline deadline);
  const ProcessResult& Result() const noexcept { return result_; }
  // 成功场景显式删除自产目录；失败场景保留文件供诊断。
  void RemoveDirectory();

private:
  void Pump();
  void Reap();
  void Cleanup() noexcept;
  void ClosePipes() noexcept;
  std::filesystem::path executable_;
  std::filesystem::path directory_;
  std::vector<std::string> arguments_;
  pid_t pid_ = -1;
  int output_fd_ = -1;
  int error_fd_ = -1;
  bool started_ = false;
  ProcessResult result_;
};

}  // namespace qbook::testing
