#include "ProcessHarness.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace qbook::testing {
namespace {
using namespace std::chrono_literals;
constexpr std::size_t kOutputLimit = 1024 * 1024;

[[noreturn]] void SystemError(const char* operation) {
  throw std::system_error(errno, std::generic_category(), operation);
}

void CloseFd(int& fd) noexcept {
  if (fd >= 0) ::close(std::exchange(fd, -1));
}

void Nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    SystemError("设置子进程管道失败");
}
}  // namespace

ProcessHarness::ProcessHarness(std::filesystem::path executable,
                               std::vector<std::string> arguments)
    : executable_(std::filesystem::absolute(executable)), arguments_(std::move(arguments)) {
  std::array<char, 40> pattern{};
  constexpr char prefix[] = "/tmp/qbook-service-XXXXXX";
  std::copy(std::begin(prefix), std::end(prefix), pattern.begin());
  const char* created = ::mkdtemp(pattern.data());
  if (!created) SystemError("创建子进程工作目录失败");
  directory_ = created;
  result_.output.reserve(kOutputLimit);
}

ProcessHarness::~ProcessHarness() noexcept {
  Cleanup();
  ClosePipes();
}

void ProcessHarness::WriteConfig(std::string_view yaml) {
  if (started_) throw std::logic_error("启动后禁止修改测试配置");
  std::filesystem::create_directory(directory_ / "config");
  std::ofstream file(directory_ / "config/config.yaml", std::ios::binary);
  file.exceptions(std::ios::badbit | std::ios::failbit);
  file.write(yaml.data(), static_cast<std::streamsize>(yaml.size()));
  file.close();
}

void ProcessHarness::Start() {
  if (started_) throw std::logic_error("子进程只能启动一次");
  const std::string executable = executable_.string();
  const std::string cwd = directory_.string();
  std::vector<char*> argv;
  argv.reserve(arguments_.size() + 2);
  argv.push_back(const_cast<char*>(executable.c_str()));
  for (auto& argument : arguments_) argv.push_back(argument.data());
  argv.push_back(nullptr);
  std::vector<std::string> environment;
  for (char** entry = environ; *entry; ++entry) environment.emplace_back(*entry);
  std::vector<char*> envp;
  envp.reserve(environment.size() + 1);
  for (auto& entry : environment) envp.push_back(entry.data());
  envp.push_back(nullptr);
  // fork 前固定所有 C++ 对象和指针；子分支只执行 async-signal-safe 系统调用。
  char* const* const argv_data = argv.data();
  char* const* const envp_data = envp.data();
  const char* const executable_data = executable.c_str();
  const char* const cwd_data = cwd.c_str();
  int output[2]{-1, -1};
  int errors[2]{-1, -1};
  try {
    if (::pipe2(output, O_CLOEXEC) < 0) SystemError("创建输出管道失败");
    if (::pipe2(errors, O_CLOEXEC) < 0) SystemError("创建启动错误管道失败");
    Nonblocking(output[0]);
    Nonblocking(errors[0]);
    const pid_t child = ::fork();
    if (child < 0) SystemError("fork 失败");
    if (child == 0) {
      ::close(output[0]);
      ::close(errors[0]);
      if (::chdir(cwd_data) == 0 &&
          ::dup2(output[1], STDOUT_FILENO) >= 0 &&
          ::dup2(output[1], STDERR_FILENO) >= 0) {
        ::close(output[1]);
        ::execve(executable_data, argv_data, envp_data);
      }
      const int error = errno;
      const auto ignored = ::write(errors[1], &error, sizeof(error));
      (void)ignored;
      ::_exit(127);
    }
    pid_ = child;
    started_ = true;
    CloseFd(output[1]);
    CloseFd(errors[1]);
    output_fd_ = std::exchange(output[0], -1);
    error_fd_ = std::exchange(errors[0], -1);
  } catch (...) {
    for (int& fd : output) CloseFd(fd);
    for (int& fd : errors) CloseFd(fd);
    throw;
  }
}

void ProcessHarness::Reap() {
  if (pid_ <= 0) return;
  int status = 0;
  const pid_t waited = ::waitpid(pid_, &status, WNOHANG);
  if (waited < 0) {
    if (errno == EINTR) return;
    SystemError("回收子进程失败");
  }
  if (waited == 0) return;
  pid_ = -1;
  if (WIFEXITED(status)) result_.exit_code = WEXITSTATUS(status);
  if (WIFSIGNALED(status)) result_.term_signal = WTERMSIG(status);
}

void ProcessHarness::Pump() {
  std::array<char, 8192> buffer{};
  for (int attempt = 0; output_fd_ >= 0 && attempt < 64; ++attempt) {
    const auto bytes = ::read(output_fd_, buffer.data(), buffer.size());
    if (bytes > 0) {
      const auto size = static_cast<std::size_t>(bytes);
      const auto retained = std::min(size, kOutputLimit - result_.output.size());
      result_.output.append(buffer.data(), retained);
      if (retained != size) result_.output_overflow = true;
    } else if (bytes == 0) {
      CloseFd(output_fd_);
    } else if (errno == EINTR) {
      continue;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    } else {
      SystemError("读取子进程输出失败");
    }
  }
  if (error_fd_ >= 0) {
    int error = 0;
    const auto bytes = ::read(error_fd_, &error, sizeof(error));
    if (bytes == sizeof(error)) result_.exec_error = error;
    if (bytes == 0 || bytes == sizeof(error)) CloseFd(error_fd_);
    else if (bytes > 0) throw std::runtime_error("子进程启动错误记录不完整");
    else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
      SystemError("读取子进程启动错误失败");
  }
  Reap();
}

void ProcessHarness::WaitForOutput(std::string_view marker, Deadline deadline) {
  if (!started_) throw std::logic_error("子进程尚未启动");
  for (;;) {
    Pump();
    if (result_.output.find(marker) != std::string::npos) return;
    if (pid_ <= 0) throw std::runtime_error("子进程在就绪前结束：" + result_.output);
    if (std::chrono::steady_clock::now() >= deadline) {
      result_.timed_out = true;
      Cleanup();
      throw std::runtime_error("等待子进程就绪超时：" + result_.output);
    }
    pollfd fd{output_fd_, POLLIN, 0};
    ::poll(&fd, 1, 5);
  }
}

void ProcessHarness::SendSignal(int signal) {
  Reap();
  if (pid_ <= 0) throw std::logic_error("已回收的子进程不能接收信号");
  if (::kill(pid_, signal) < 0) SystemError("发送子进程信号失败");
}

const ProcessResult& ProcessHarness::Wait(Deadline deadline) {
  if (!started_) throw std::logic_error("子进程尚未启动");
  while (pid_ > 0 || output_fd_ >= 0 || error_fd_ >= 0) {
    Pump();
    if (pid_ <= 0 && output_fd_ < 0 && error_fd_ < 0) break;
    if (std::chrono::steady_clock::now() >= deadline) {
      result_.timed_out = true;
      Cleanup();
      break;
    }
    pollfd fd{output_fd_, POLLIN, 0};
    ::poll(&fd, 1, 5);
  }
  return result_;
}

void ProcessHarness::Cleanup() noexcept {
  if (pid_ <= 0) return;
  ::kill(pid_, SIGTERM);
  const auto deadline = std::chrono::steady_clock::now() + 200ms;
  while (pid_ > 0 && std::chrono::steady_clock::now() < deadline) {
    try { Pump(); } catch (...) { break; }
    std::this_thread::sleep_for(2ms);
  }
  if (pid_ > 0) {
    ::kill(pid_, SIGKILL);
    int status = 0;
    pid_t waited;
    do { waited = ::waitpid(pid_, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited > 0) {
      if (WIFEXITED(status)) result_.exit_code = WEXITSTATUS(status);
      if (WIFSIGNALED(status)) result_.term_signal = WTERMSIG(status);
      pid_ = -1;
    }
  }
  try { Pump(); } catch (...) {}
}

void ProcessHarness::ClosePipes() noexcept {
  CloseFd(output_fd_);
  CloseFd(error_fd_);
}

void ProcessHarness::RemoveDirectory() {
  if (pid_ > 0) throw std::logic_error("删除工作目录前须回收子进程");
  std::filesystem::remove_all(directory_);
}

}  // namespace qbook::testing
