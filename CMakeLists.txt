cmake_minimum_required(VERSION 3.28)
project(qbook VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
option(QBOOK_BUILD_PIPELINE "构建 Feed 与 Book 流水线" ON)
option(QBOOK_BUILD_SERVICE "构建完整 qbook 服务" OFF)
option(QBOOK_BUILD_BENCHMARKS "构建性能实验" OFF)
if(QBOOK_BUILD_SERVICE)
  message(FATAL_ERROR "当前版本尚未实现 qbook 服务；请设置 QBOOK_BUILD_SERVICE=OFF")
endif()
if(QBOOK_BUILD_BENCHMARKS)
  message(FATAL_ERROR "当前版本尚未实现 benchmark；请设置 QBOOK_BUILD_BENCHMARKS=OFF")
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
  src/Threads/MetricsThread.cpp)
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

  if(QBOOK_BUILD_PIPELINE)
    add_executable(qbook_pipeline_tests tests/feed_tests.cpp tests/book_thread_tests.cpp
      tests/pipeline_tests.cpp tests/support/FeedPublisher.cpp tests/support/RecordCollector.cpp)
    target_link_libraries(qbook_pipeline_tests PRIVATE qbook_pipeline qbook_logging Catch2::Catch2WithMain)
    target_compile_options(qbook_pipeline_tests PRIVATE -Wall -Wextra -Wpedantic)
    catch_discover_tests(qbook_pipeline_tests TEST_PREFIX "pipeline::" PROPERTIES TIMEOUT 30)
  endif()
endif()
