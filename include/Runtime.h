#pragma once

#include <atomic>
#include <exception>

namespace qbook {

struct Config;

// 主线程独占运行一次服务；配置须已验证，所有阶段结束后返回正常 0 或失败非零。
int RunService(const Config& config);

// 保存一次运行的首个致命故障；对象须存活到所有借用线程结束。
class RunControl {
public:
  RunControl() noexcept = default;
  RunControl(const RunControl&) = delete;
  RunControl& operator=(const RunControl&) = delete;
  RunControl(RunControl&&) = delete;
  RunControl& operator=(RunControl&&) = delete;

  // 允许并发报告；仅原子交换的获胜者保存诊断。
  // stage 须指向非空静态阶段名，error 须为已捕获的非空异常。
  void Fail(const char* stage, std::exception_ptr error) noexcept;

  // 返回失败提示；true 仅供退出检查，不表示诊断已完成写入。
  bool Failed() const noexcept;

  // 返回异常句柄副本；调用前须 join 所有潜在报告线程。
  // 无并发报告者时可直接读取；初始值为空。
  std::exception_ptr Error() const noexcept;

  // 返回保存的静态阶段名；读取前提同 Error，初始值为 nullptr。
  const char* Stage() const noexcept;

private:
  std::atomic<bool> failed_{false};
  std::exception_ptr error_{};
  const char* stage_ = nullptr;
};

}  // namespace qbook
