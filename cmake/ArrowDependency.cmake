include_guard(GLOBAL)

# 使用开发环境提供的固定版本；不自动选择仓库内的 .deps 安装。
function(qbook_find_arrow_dependencies)
    find_package(Arrow 26.0.0 EXACT CONFIG REQUIRED)
    find_package(Parquet 26.0.0 EXACT CONFIG REQUIRED)
    if(NOT TARGET Arrow::arrow_shared OR NOT TARGET Parquet::parquet_shared)
        message(FATAL_ERROR "Arrow/Parquet 26.0.0 安装必须提供官方 shared targets")
    endif()

    add_library(qbook_arrow_dependencies INTERFACE)
    target_link_libraries(qbook_arrow_dependencies INTERFACE
        Arrow::arrow_shared Parquet::parquet_shared)

    message(STATUS "qbook Arrow ${ARROW_VERSION}: ${Arrow_DIR}")
    message(STATUS "qbook Parquet ${PARQUET_VERSION}: ${Parquet_DIR}")
endfunction()

qbook_find_arrow_dependencies()
