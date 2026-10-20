cmake_minimum_required(VERSION 3.28)
project(qlib_consumer_v0 LANGUAGES CXX)

if(NOT DEFINED QLIB_SOURCE_DIR)
  message(FATAL_ERROR "请通过 QLIB_SOURCE_DIR 指定 qlib 源码目录")
endif()

# 标准和头文件路径由 qlib::qlib 传入，调用工程不自行补齐。
add_subdirectory("${QLIB_SOURCE_DIR}" qlib-build)
add_executable(qlib_consumer_v0 main.cpp)
target_link_libraries(qlib_consumer_v0 PRIVATE qlib::qlib)

# 独立配置统一覆盖宏，验证头文件保留调用方指定值。
if(DEFINED QLIB_CONSUMER_CACHE_LINE)
  target_compile_definitions(qlib_consumer_v0 PRIVATE
    QLIB_CACHE_LINE=${QLIB_CONSUMER_CACHE_LINE}
    EXPECTED_CACHE_LINE=${QLIB_CONSUMER_CACHE_LINE})
else()
  target_compile_definitions(qlib_consumer_v0 PRIVATE EXPECTED_CACHE_LINE=64)
endif()
