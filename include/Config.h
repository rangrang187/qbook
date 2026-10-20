#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace qbook {

// 保存启动参数；默认构造只初始化值，加载和验证由配套函数完成。
struct Config {
  std::string feed_endpoint = "tcp://127.0.0.1:5555";
  bool l2 = false;
  std::size_t feed_queue_capacity = 16384;
  std::chrono::milliseconds snapshot_period{3000};
  std::size_t record_queue_capacity = 16384;
  std::filesystem::path out_dir = "data_out";
  std::int64_t batch_rows = 1024;
  std::string metrics_endpoint = "tcp://127.0.0.1:5556";
  std::chrono::milliseconds sample_period{100};
};

// 读取平坦 YAML 并返回已验证值；缺文件记录警告，I/O 或配置错误抛异常。
Config LoadConfig(const std::filesystem::path& path);

// 校验字段和后续运算的可表示范围；失败抛 invalid_argument，保留输入值。
void ValidateConfig(const Config& config);

}  // namespace qbook
