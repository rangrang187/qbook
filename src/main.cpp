#include "Config.h"
#include "Runtime.h"

#include <spdlog/spdlog.h>

#include <exception>

int main() {
  try {
    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::info);
    const qbook::Config config = qbook::LoadConfig("config/config.yaml");
    return qbook::RunService(config);
  } catch (const std::exception& error) {
    try { spdlog::critical("启动失败：{}", error.what()); } catch (...) {}
  } catch (...) {
    try { spdlog::critical("启动失败：未知异常"); } catch (...) {}
  }
  return 1;
}
