#include "support/MetricsFixtures.h"

#include <cerrno>
#include <filesystem>
#include <iostream>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::invalid_argument("usage: qbook_metrics_fixture_generator OUTPUT_DIRECTORY");
    const std::filesystem::path directory{argv[1]};
    std::filesystem::create_directories(directory);
    for (const auto name : qbook::testing::metrics_fixture_names) {
      const auto sample = qbook::testing::MetricsFixture(name);
      const auto path = directory / (std::string(name) + ".bin");
      const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
      if (fd < 0) throw std::system_error(errno, std::generic_category(), "exclusive fixture open");
      std::size_t offset = 0;
      while (offset < sizeof(sample)) {
        const auto n = ::write(fd, reinterpret_cast<const char*>(&sample) + offset, sizeof(sample) - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
          const int error = n < 0 ? errno : EIO;
          ::close(fd);
          throw std::system_error(error, std::generic_category(), "fixture write");
        }
        offset += static_cast<std::size_t>(n);
      }
      if (::close(fd) < 0) throw std::system_error(errno, std::generic_category(), "fixture close");
      std::cout << name << ' ' << sizeof(sample) << '\n';
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
