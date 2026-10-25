include_guard(GLOBAL)

# 查找同一安装中的固定 Arrow/Parquet，并传递本地运行库路径。
function(qbook_find_arrow_dependencies)
    get_filename_component(local_prefix
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../.deps/arrow-26.0.0/usr" ABSOLUTE)
    set(local_arrow_dir "${local_prefix}/lib/x86_64-linux-gnu/cmake/Arrow")
    set(use_local FALSE)
    set(had_library_path FALSE)
    if(EXISTS "${local_arrow_dir}/ArrowConfig.cmake")
        if(NOT Arrow_DIR OR Arrow_DIR STREQUAL local_arrow_dir)
            set(use_local TRUE)
            list(PREPEND CMAKE_PREFIX_PATH "${local_prefix}")
            # Ubuntu 的 SnappyConfig 固定使用 /usr；Arrow 自带模块可查找本地文件。
            set(CMAKE_DISABLE_FIND_PACKAGE_Snappy TRUE)
            # 这些包通过 Arrow 的查找模块提供真实头文件和库。
            set(CMAKE_DISABLE_FIND_PACKAGE_lz4 TRUE)
            set(CMAKE_DISABLE_FIND_PACKAGE_re2 TRUE)
            set(CMAKE_DISABLE_FIND_PACKAGE_Thrift TRUE)
            set(CMAKE_DISABLE_FIND_PACKAGE_protobuf TRUE)
            set(runtime_dir "${local_prefix}/lib/x86_64-linux-gnu")
            # FindProtobuf 会运行 protoc；探测期间补齐其共享库搜索路径。
            if(DEFINED ENV{LD_LIBRARY_PATH})
                set(had_library_path TRUE)
                set(saved_library_path "$ENV{LD_LIBRARY_PATH}")
            endif()
            if(had_library_path AND NOT saved_library_path STREQUAL "")
                set(ENV{LD_LIBRARY_PATH} "${runtime_dir}:${saved_library_path}")
            else()
                set(ENV{LD_LIBRARY_PATH} "${runtime_dir}")
            endif()
        endif()
    endif()

    find_package(Arrow 26.0.0 EXACT CONFIG REQUIRED)
    find_package(Parquet 26.0.0 EXACT CONFIG REQUIRED)
    if(use_local)
        if(had_library_path)
            set(ENV{LD_LIBRARY_PATH} "${saved_library_path}")
        else()
            unset(ENV{LD_LIBRARY_PATH})
        endif()
    endif()
    if(NOT TARGET Arrow::arrow_shared OR NOT TARGET Parquet::parquet_shared)
        message(FATAL_ERROR "Arrow/Parquet 26.0.0 安装必须提供官方 shared targets")
    endif()

    add_library(qbook_arrow_dependencies INTERFACE)
    target_link_libraries(qbook_arrow_dependencies INTERFACE
        Arrow::arrow_shared Parquet::parquet_shared)

    if(use_local)
        cmake_path(IS_PREFIX local_prefix "${Arrow_DIR}" NORMALIZE arrow_is_local)
        cmake_path(IS_PREFIX local_prefix "${Parquet_DIR}" NORMALIZE parquet_is_local)
        if(NOT arrow_is_local OR NOT parquet_is_local)
            message(FATAL_ERROR "本地 Arrow/Parquet 必须来自同一 .deps/arrow-26.0.0 安装")
        endif()
        # ELF RPATH 传递到间接依赖，供测试发现和正式程序解析本地共享库。
        target_link_options(qbook_arrow_dependencies INTERFACE
            "LINKER:--disable-new-dtags" "LINKER:-rpath,${runtime_dir}")
        set(QBOOK_ARROW_RUNTIME_DIR "${runtime_dir}" PARENT_SCOPE)
    endif()

    set(ARROW_VERSION "${ARROW_VERSION}" PARENT_SCOPE)
    set(PARQUET_VERSION "${PARQUET_VERSION}" PARENT_SCOPE)
endfunction()

qbook_find_arrow_dependencies()
