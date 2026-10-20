cmake_minimum_required(VERSION 3.28)
project(qlib_consumer_v4 LANGUAGES CXX)

if(NOT DEFINED QLIB_SOURCE_DIR)
  message(FATAL_ERROR "请通过 QLIB_SOURCE_DIR 指定 qlib 源码目录")
endif()

# 测试开关采用库的默认值，标准和头文件路径由接口目标传入。
add_subdirectory("${QLIB_SOURCE_DIR}" qlib-build)
add_executable(qlib_consumer_v4
  main.cpp
  check_common.cpp
  check_hive.cpp
  check_map.cpp
  check_single_queue.cpp
  check_pool.cpp
  check_memory_pool.cpp)
target_link_libraries(qlib_consumer_v4 PRIVATE qlib::qlib)
set_target_properties(qlib_consumer_v4 PROPERTIES
  UNITY_BUILD OFF
  DISABLE_PRECOMPILE_HEADERS ON)
