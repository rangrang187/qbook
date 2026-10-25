#include "OutputStreams.h"

#include <arrow/io/file.h>

#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace qbook::detail {
namespace {
struct OwnedDescriptor {
  int value;
  ~OwnedDescriptor() { if (value >= 0) ::close(value); }
};
}

std::shared_ptr<arrow::io::OutputStream> OpenExclusiveOutput(const std::filesystem::path& path) {
  OwnedDescriptor descriptor{::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644)};
  if (descriptor.value < 0)
    throw std::system_error(errno, std::generic_category(), path.string() + " / exclusive open");
  auto result = arrow::io::FileOutputStream::Open(descriptor.value);
  if (!result.ok()) throw std::runtime_error(path.string() + " / Arrow fd open: " + result.status().ToString());
  // Arrow 26 的 fd 重载在全部准备成功后取得所有权。
  descriptor.value = -1;
  return std::move(result).ValueOrDie();
}
}  // namespace qbook::detail
