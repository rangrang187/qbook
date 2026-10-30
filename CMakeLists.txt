cmake_minimum_required(VERSION 3.28)
project(qbook VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
option(QBOOK_BUILD_PIPELINE "构建 Feed 与 Book 流水线" ON)
option(QBOOK_BUILD_PERSISTENCE "构建 Arrow/Parquet 存储与 Writer 阶段" OFF)
option(QBOOK_BUILD_SERVICE "构建完整 qbook 服务" OFF)
option(QBOOK_BUILD_BENCHMARKS "构建性能实验" OFF)
if(QBOOK_BUILD_SERVICE AND (NOT QBOOK_BUILD_PIPELINE OR NOT QBOOK_BUILD_PERSISTENCE))
  message(FATAL_ERROR "QBOOK_BUILD_SERVICE=ON 要求显式同时开启 QBOOK_BUILD_PIPELINE 和 QBOOK_BUILD_PERSISTENCE")
endif()

# 依赖源码及内容清单随工程保留，配置时无需联网。
foreach(required_file IN ITEMS
    third_party/qlib/CMakeLists.txt
    third_party/qlib/include/qlib/common.h
    third_party/qlib/include/qlib/single_queue.h
    third_party/qlib/LICENSE
    third_party/fkYAML/include/fkYAML/node.hpp
    third_party/fkYAML/LICENSE.txt
    third_party/spdlog/include/spdlog/spdlog.h
    third_party/spdlog/include/spdlog/LICENSE.txt
    third_party/spdlog/include/spdlog/fmt/bundled/fmt.license.rst)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${required_file}")
    message(FATAL_ERROR "缺少固定依赖文件：${required_file}；请按 README 恢复依赖源码")
  endif()
endforeach()
set(QLIB_BUILD_TESTS OFF CACHE BOOL "qbook 不构建 qlib 自带测试" FORCE)
add_subdirectory(third_party/qlib)
if(NOT TARGET qlib::qlib)
  message(FATAL_ERROR "固定 qlib 源码未提供 qlib::qlib 目标")
endif()

add_library(qbook_yaml INTERFACE)
target_include_directories(qbook_yaml SYSTEM INTERFACE third_party/fkYAML/include)
add_library(qbook_logging INTERFACE)
target_include_directories(qbook_logging SYSTEM INTERFACE third_party/spdlog/include)
# spdlog 默认使用 header-only 与 bundled fmt，所有调用方共用此目标。
find_package(Threads REQUIRED)
target_link_libraries(qbook_logging INTERFACE Threads::Threads)

add_library(qbook_support STATIC
  src/Config.cpp
  src/Runtime.cpp
  src/Metrics.cpp)
target_include_directories(qbook_support PUBLIC include)
target_link_libraries(qbook_support PUBLIC qlib::qlib PRIVATE qbook_yaml qbook_logging)
target_compile_options(qbook_support PRIVATE -Wall -Wextra -Wpedantic)

add_library(qbook_core STATIC src/Orderbook.cpp)
target_include_directories(qbook_core PUBLIC include)
target_compile_features(qbook_core PUBLIC cxx_std_23)
target_link_libraries(qbook_core PUBLIC qlib::qlib)
target_compile_options(qbook_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(qbook_protocol STATIC src/Protocol.cpp)
target_include_directories(qbook_protocol PUBLIC include)
target_compile_features(qbook_protocol PUBLIC cxx_std_23)
target_link_libraries(qbook_protocol PUBLIC qlib::qlib)
target_compile_options(qbook_protocol PRIVATE -Wall -Wextra -Wpedantic)

if(QBOOK_BUILD_PIPELINE)
  # 官方固定源码：libzmq v4.3.5、cppzmq v4.10.0，内容清单见 third_party/*.sha256。
  # 关闭流水线时不进入此分支，也不探测通信依赖。
  enable_language(C)
  foreach(required_file IN ITEMS third_party/libzmq/CMakeLists.txt
      third_party/libzmq/include/zmq.h third_party/cppzmq/CMakeLists.txt
      third_party/cppzmq/zmq.hpp)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${required_file}")
      message(FATAL_ERROR "缺少固定通信依赖：${required_file}；请按 README 恢复源码")
    endif()
  endforeach()
  set(BUILD_SHARED ON CACHE BOOL "构建固定 libzmq 动态库" FORCE)
  set(BUILD_STATIC OFF CACHE BOOL "只构建当前使用的 libzmq 动态目标" FORCE)
  foreach(dependency_option IN ITEMS BUILD_TESTS WITH_DOC WITH_DOCS WITH_PERF_TOOL
      WITH_LIBBSD WITH_LIBSODIUM WITH_OPENPGM WITH_NORM ENABLE_CURVE ENABLE_DRAFTS
      ENABLE_WS ENABLE_CPACK CPPZMQ_BUILD_TESTS)
    set(${dependency_option} OFF CACHE BOOL "qbook 固定通信依赖选项" FORCE)
  endforeach()
  add_subdirectory(third_party/libzmq EXCLUDE_FROM_ALL)
  add_subdirectory(third_party/cppzmq EXCLUDE_FROM_ALL)
  if(NOT TARGET libzmq OR NOT TARGET cppzmq)
    message(FATAL_ERROR "固定通信源码未提供 libzmq/cppzmq 真实目标")
  endif()
  add_library(qbook_pipeline STATIC src/Threads/FeedThread.cpp src/Threads/BookThread.cpp)
  target_include_directories(qbook_pipeline PUBLIC include)
  target_compile_features(qbook_pipeline PUBLIC cxx_std_23)
  target_link_libraries(qbook_pipeline PUBLIC qbook_core qbook_support qbook_protocol
    cppzmq Threads::Threads PRIVATE qbook_logging)
  target_compile_options(qbook_pipeline PRIVATE -Wall -Wextra -Wpedantic)
  add_library(qbook_metrics STATIC src/Threads/MetricsThread.cpp)
  target_include_directories(qbook_metrics PUBLIC include PRIVATE src)
  target_link_libraries(qbook_metrics PUBLIC qbook_support
    PRIVATE cppzmq Threads::Threads qbook_logging)
  target_compile_options(qbook_metrics PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(QBOOK_BUILD_PERSISTENCE)
  include(cmake/ArrowDependency.cmake)
  add_library(qbook_storage STATIC src/ParquetWriter.cpp src/detail/OutputStreams.cpp)
  target_include_directories(qbook_storage PUBLIC include PRIVATE src)
  target_link_libraries(qbook_storage PUBLIC qlib::qlib
    PRIVATE qbook_arrow_dependencies qbook_logging)
  target_compile_options(qbook_storage PRIVATE -Wall -Wextra -Wpedantic)

  add_library(qbook_writer STATIC src/Threads/WriterThread.cpp src/detail/RunDirectory.cpp)
  target_include_directories(qbook_writer PUBLIC include PRIVATE src)
  target_link_libraries(qbook_writer PUBLIC qbook_support qbook_storage Threads::Threads
    PRIVATE qbook_logging)
  target_compile_options(qbook_writer PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(QBOOK_BUILD_SERVICE)
  add_library(qbook_service STATIC src/Service.cpp)
  target_include_directories(qbook_service PUBLIC include PRIVATE src)
  target_link_libraries(qbook_service PUBLIC qbook_pipeline qbook_writer qbook_metrics PRIVATE qbook_logging)
  target_compile_options(qbook_service PRIVATE -Wall -Wextra -Wpedantic)
  add_executable(qbook src/main.cpp)
  target_link_libraries(qbook PRIVATE qbook_service qbook_logging)
  target_compile_options(qbook PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(QBOOK_BUILD_BENCHMARKS)
  set(qbook_benchmark_targets qbook_core_bench qbook_protocol_bench)
  # 实验工具与生产库分离；核心/协议实验不引入通信、Arrow 或 Catch2。
  add_library(qbook_benchmark_support STATIC benchmarks/support/Workloads.cpp
    benchmarks/support/Measure.cpp benchmarks/support/Results.cpp)
  target_include_directories(qbook_benchmark_support PUBLIC benchmarks/support)
  target_link_libraries(qbook_benchmark_support PUBLIC qlib::qlib PRIVATE qbook_yaml)
  target_compile_definitions(qbook_benchmark_support PRIVATE
    QBOOK_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    QBOOK_BUILD_DIR="${CMAKE_BINARY_DIR}")
  target_compile_options(qbook_benchmark_support PRIVATE -Wall -Wextra -Wpedantic)

  add_library(qbook_core_benchmark_support STATIC benchmarks/support/CoreTrial.cpp)
  target_include_directories(qbook_core_benchmark_support PRIVATE tests)
  target_link_libraries(qbook_core_benchmark_support PUBLIC qbook_benchmark_support qbook_core)
  add_executable(qbook_core_bench benchmarks/core_bench.cpp)
  target_link_libraries(qbook_core_bench PRIVATE qbook_core_benchmark_support)

  add_library(qbook_protocol_benchmark_support STATIC benchmarks/support/ProtocolTrial.cpp)
  target_include_directories(qbook_protocol_benchmark_support PRIVATE tests)
  target_compile_definitions(qbook_protocol_benchmark_support PRIVATE
    QBOOK_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    QBOOK_PROTOCOL_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/protocol")
  target_link_libraries(qbook_protocol_benchmark_support PUBLIC qbook_benchmark_support qbook_protocol)
  add_executable(qbook_protocol_bench benchmarks/protocol_bench.cpp)
  target_link_libraries(qbook_protocol_bench PRIVATE qbook_protocol_benchmark_support)

  if(QBOOK_BUILD_PERSISTENCE)
    # 独立读回器只含 Arrow 核对逻辑，不链接测试断言或分配替换。
    add_library(qbook_benchmark_readback STATIC tests/support/ParquetReadback.cpp)
    target_include_directories(qbook_benchmark_readback PUBLIC tests)
    target_link_libraries(qbook_benchmark_readback PUBLIC qlib::qlib qbook_arrow_dependencies)
    add_library(qbook_writer_benchmark_support STATIC benchmarks/support/WriterTrial.cpp)
    target_include_directories(qbook_writer_benchmark_support PUBLIC src)
    target_link_libraries(qbook_writer_benchmark_support PUBLIC qbook_benchmark_support
      qbook_storage qbook_benchmark_readback)
    add_executable(qbook_writer_bench benchmarks/writer_bench.cpp)
    target_link_libraries(qbook_writer_bench PRIVATE qbook_writer_benchmark_support)
    list(APPEND qbook_benchmark_targets qbook_writer_bench)
  endif()

  if(QBOOK_BUILD_SERVICE)
    add_library(qbook_pipeline_benchmark_support STATIC benchmarks/support/PipelineTrial.cpp
      tests/support/ProcessHarness.cpp)
    target_include_directories(qbook_pipeline_benchmark_support PUBLIC src tests)
    target_link_libraries(qbook_pipeline_benchmark_support PUBLIC qbook_benchmark_support
      qbook_service qbook_benchmark_readback PRIVATE qbook_yaml qbook_logging)
    add_executable(qbook_pipeline_bench benchmarks/pipeline_bench.cpp)
    target_link_libraries(qbook_pipeline_bench PRIVATE qbook_pipeline_benchmark_support)
    list(APPEND qbook_benchmark_targets qbook_pipeline_bench)
  endif()
  message(STATUS "qbook benchmark targets: ${qbook_benchmark_targets}")
endif()

if(BUILD_TESTING)
  # Catch2 v3.8.1，提交 2b60af89e23d28eefc081bc930831ee9d45ea58b。
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Catch2/extras/Catch.cmake")
    message(FATAL_ERROR "缺少固定 Catch2 v3 源码；请按 README 恢复 third_party/Catch2")
  endif()
  set(CATCH_DEVELOPMENT_BUILD OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/Catch2 EXCLUDE_FROM_ALL)
  add_executable(qbook_tests
    tests/contracts_tests.cpp
    tests/config_tests.cpp
    tests/runtime_metrics_tests.cpp
    tests/orderbook_add_tests.cpp
    tests/orderbook_modify_tests.cpp
    tests/orderbook_reduce_tests.cpp
    tests/orderbook_l2_tests.cpp
    tests/orderbook_invariant_tests.cpp
    tests/protocol_tests.cpp)
  target_compile_definitions(qbook_tests PRIVATE
    QBOOK_PROTOCOL_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/protocol")
  target_link_libraries(qbook_tests PRIVATE qbook_support qbook_core qbook_protocol qbook_logging
    Catch2::Catch2WithMain Threads::Threads)
  target_compile_options(qbook_tests PRIVATE -Wall -Wextra -Wpedantic)
  include(third_party/Catch2/extras/Catch.cmake)
  catch_discover_tests(qbook_tests TEST_PREFIX "unit::" PROPERTIES TIMEOUT 30)

  # 分配替换只链接到专用进程；两个程序共用同一份生产核心库。
  add_executable(qbook_allocation_tests
    tests/orderbook_allocation_tests.cpp
    tests/support/AllocationFailure.cpp)
  target_link_libraries(qbook_allocation_tests PRIVATE qbook_core Catch2::Catch2WithMain)
  target_compile_options(qbook_allocation_tests PRIVATE -Wall -Wextra -Wpedantic)
  catch_discover_tests(qbook_allocation_tests TEST_PREFIX "allocation::"
    PROPERTIES TIMEOUT 30)

  if(QBOOK_BUILD_BENCHMARKS)
    add_executable(qbook_benchmark_checks tests/benchmark_checks.cpp)
    target_include_directories(qbook_benchmark_checks PRIVATE tests)
    target_link_libraries(qbook_benchmark_checks PRIVATE qbook_core_benchmark_support
      qbook_protocol_benchmark_support Catch2::Catch2WithMain)
    if(QBOOK_BUILD_PERSISTENCE)
      target_sources(qbook_benchmark_checks PRIVATE tests/writer_benchmark_checks.cpp
        tests/support/FailingOutputStream.cpp)
      target_link_libraries(qbook_benchmark_checks PRIVATE qbook_writer_benchmark_support qbook_support)
    endif()
    if(QBOOK_BUILD_SERVICE)
      target_sources(qbook_benchmark_checks PRIVATE tests/pipeline_benchmark_checks.cpp)
      target_link_libraries(qbook_benchmark_checks PRIVATE qbook_pipeline_benchmark_support)
    endif()
    target_compile_options(qbook_benchmark_checks PRIVATE -Wall -Wextra -Wpedantic)
    catch_discover_tests(qbook_benchmark_checks TEST_PREFIX "benchmark::" PROPERTIES TIMEOUT 60)
  endif()

  add_executable(qbook_metrics_fixture_generator tests/metrics_fixture_generator.cpp)
  target_include_directories(qbook_metrics_fixture_generator PRIVATE src tests)
  target_link_libraries(qbook_metrics_fixture_generator PRIVATE qlib::qlib)
  target_compile_options(qbook_metrics_fixture_generator PRIVATE -Wall -Wextra -Wpedantic)

  if(QBOOK_BUILD_PIPELINE)
    add_executable(qbook_pipeline_tests tests/feed_tests.cpp tests/book_thread_tests.cpp
      tests/pipeline_tests.cpp tests/support/FeedPublisher.cpp tests/support/RecordCollector.cpp)
    target_link_libraries(qbook_pipeline_tests PRIVATE qbook_pipeline qbook_logging Catch2::Catch2WithMain)
    target_compile_options(qbook_pipeline_tests PRIVATE -Wall -Wextra -Wpedantic)
    catch_discover_tests(qbook_pipeline_tests TEST_PREFIX "pipeline::" PROPERTIES TIMEOUT 30)
    add_executable(qbook_metrics_tests tests/metrics_tests.cpp tests/metrics_integration_tests.cpp)
    target_include_directories(qbook_metrics_tests PRIVATE src tests)
    target_link_libraries(qbook_metrics_tests PRIVATE qbook_metrics cppzmq Catch2::Catch2WithMain)
    target_compile_options(qbook_metrics_tests PRIVATE -Wall -Wextra -Wpedantic)
    catch_discover_tests(qbook_metrics_tests TEST_PREFIX "metrics::" PROPERTIES TIMEOUT 20)

    add_executable(qbook_monitoring_fixture_publisher tests/monitoring_fixture_publisher.cpp)
    target_include_directories(qbook_monitoring_fixture_publisher PRIVATE include src tests)
    target_link_libraries(qbook_monitoring_fixture_publisher PRIVATE qlib::qlib cppzmq Threads::Threads)
    target_compile_options(qbook_monitoring_fixture_publisher PRIVATE -Wall -Wextra -Wpedantic)
  endif()

  if(QBOOK_BUILD_PERSISTENCE)
    # 不含全局分配替换；文件读取和故障流仅供这两个验收程序使用。
    add_library(qbook_storage_test_support STATIC tests/support/ParquetReadback.cpp
      tests/support/FailingOutputStream.cpp)
    target_include_directories(qbook_storage_test_support PUBLIC tests src)
    target_link_libraries(qbook_storage_test_support PUBLIC qbook_storage qbook_support
      qbook_arrow_dependencies)
    target_compile_options(qbook_storage_test_support PRIVATE -Wall -Wextra -Wpedantic)
    add_executable(qbook_storage_tests tests/parquet_writer_tests.cpp tests/writer_thread_tests.cpp)
    target_link_libraries(qbook_storage_tests PRIVATE qbook_writer qbook_storage_test_support
      Catch2::Catch2WithMain)
    target_compile_options(qbook_storage_tests PRIVATE -Wall -Wextra -Wpedantic)
    catch_discover_tests(qbook_storage_tests TEST_PREFIX "storage::" PROPERTIES TIMEOUT 45)
    add_executable(qbook_parquet_verify tests/parquet_verify.cpp)
    target_link_libraries(qbook_parquet_verify PRIVATE qbook_storage_test_support)
    target_compile_options(qbook_parquet_verify PRIVATE -Wall -Wextra -Wpedantic)
  endif()

  if(QBOOK_BUILD_SERVICE)
    add_executable(qbook_service_test_driver tests/service_test_driver.cpp)
    target_link_libraries(qbook_service_test_driver PRIVATE qbook_service
      qbook_storage_test_support qbook_logging)
    target_compile_options(qbook_service_test_driver PRIVATE -Wall -Wextra -Wpedantic)
    add_executable(qbook_service_tests tests/service_lifecycle_tests.cpp
      tests/service_integration_tests.cpp tests/support/ProcessHarness.cpp tests/support/FeedPublisher.cpp)
    target_link_libraries(qbook_service_tests PRIVATE qbook_service qbook_storage_test_support
      qbook_logging Catch2::Catch2WithMain)
    target_compile_definitions(qbook_service_tests PRIVATE
      QBOOK_EXECUTABLE="$<TARGET_FILE:qbook>"
      QBOOK_SERVICE_TEST_DRIVER="$<TARGET_FILE:qbook_service_test_driver>")
    target_compile_options(qbook_service_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_dependencies(qbook_service_tests qbook qbook_service_test_driver)
    catch_discover_tests(qbook_service_tests TEST_PREFIX "service::" PROPERTIES TIMEOUT 60)
    # UI 编排读取本次构建的绝对路径，不猜测构建目录或使用历史可执行文件。
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/tests/runtime-paths.json" CONTENT
"{\n  \"qbook\": \"$<TARGET_FILE:qbook>\",\n  \"serviceTestDriver\": \"$<TARGET_FILE:qbook_service_test_driver>\",\n  \"fixturePublisher\": \"$<TARGET_FILE:qbook_monitoring_fixture_publisher>\",\n  \"fixtureGenerator\": \"$<TARGET_FILE:qbook_metrics_fixture_generator>\",\n  \"parquetVerify\": \"$<TARGET_FILE:qbook_parquet_verify>\",\n  \"metricsFixtures\": \"${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/metrics\"\n}\n")
  endif()
endif()
