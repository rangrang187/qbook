cmake_minimum_required(VERSION 3.28)
project(qlib_consumer_v1 LANGUAGES CXX)

if(NOT DEFINED QLIB_SOURCE_DIR)
  message(FATAL_ERROR "请通过 QLIB_SOURCE_DIR 指定 qlib 源码目录")
endif()

# 调用方只链接接口目标，由它传递头文件路径和 C++23 要求。
add_subdirectory("${QLIB_SOURCE_DIR}" qlib-build)
foreach(component IN ITEMS hive map)
  add_executable(${component}_consumer ${component}_main.cpp)
  target_link_libraries(${component}_consumer PRIVATE qlib::qlib)
endforeach()

if(QLIB_BUILD_TESTS OR TARGET GTest::gtest OR TARGET gtest)
  message(FATAL_ERROR "独立接入不应默认启用 qlib 测试或 GoogleTest")
endif()
