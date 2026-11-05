# ===========================================================================
# ZhaoHua Engine —— ARM64 指令级 hook 引擎
#
# 用法：
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
#         -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
#         -DANDROID_STL=none
#   cmake --build build -j
#
# 产物：
#   build/libzh_engine.so      主引擎
#   build/libzh_engine.a       静态库（可选）
# ===========================================================================
cmake_minimum_required(VERSION 3.20)

project(zh_engine
    VERSION 1.0.0
    DESCRIPTION "ARM64 instruction-level hook engine for Minecraft Bedrock"
    LANGUAGES C)

# ---- 选项 ----------------------------------------------------------------
option(ZH_BUILD_TESTS      "Build host-side unit tests"        OFF)
option(ZH_BUILD_EXAMPLES   "Build example modules"             OFF)
option(ZH_BUILD_STATIC     "Also build static library"         OFF)
option(ZH_USE_ASAN         "Enable AddressSanitizer"           OFF)
option(ZH_WERROR           "Treat warnings as errors"          OFF)
option(ZH_STRICT           "Enable strict warnings"            OFF)
# ZH_PORTABLE：去掉 bionic 专有依赖，让产物能在普通 Linux（musl/glibc）上 dlopen。
# 默认关（Android 上原样最优）；做跨平台测试或集成时需要打开。
#   · 关掉 _FORTIFY_SOURCE  → 消掉 __memcpy_chk / __write_chk 等 7 个符号
#   · 不引用 __system_property_get（改用空实现）
option(ZH_PORTABLE         "Strip bionic-specific deps for non-Android hosts" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)   # 需要 syscall / 匿名 mmap 等 GNU 扩展

# ---- 架构 ----------------------------------------------------------------
if(NOT DEFINED ANDROID_ABI)
    # 非 Android 交叉编译时（host 测试），按本机架构
    execute_process(COMMAND uname -m OUTPUT_VARIABLE ZH_HOST_ARCH OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(ZH_HOST_ARCH STREQUAL "aarch64")
        set(ZH_ARCH "arm64")
    else()
        set(ZH_ARCH "unsupported")
    endif()
else()
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(ZH_ARCH "arm64")
    else()
        message(FATAL_ERROR "目前只支持 arm64-v8a，收到 ${ANDROID_ABI}")
    endif()
endif()

message(STATUS "zh_engine: arch = ${ZH_ARCH}")

# ---- 源文件 --------------------------------------------------------------
set(ZH_PUBLIC_HEADERS
    include/zh/instr_hook.h
    include/zh/zhreg.h
    include/zh/reloc.h
    include/zh/zlog.h
    include/zh/zh_layer.h
    include/zh/zh_db.h
    include/zh/zh_typed.h)

set(ZH_SOURCES
    src/instr_hook.c
    src/zhreg.c
    src/reloc.c
    src/zlog.c
    src/ctxdump.c
    src/zh_layer.c
    src/guard.c
    src/zh_db.c
    src/zh_tbl.c)

# 非 Android 兼容层（只在 ZH_PORTABLE=ON 时参与编译，内部有 #ifdef 保护）
list(APPEND ZH_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/common/portable.c)

if(ZH_ARCH STREQUAL "arm64")
    file(GLOB ZH_ARCH_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/arch/arm64/*.c)
    list(APPEND ZH_SOURCES ${ZH_ARCH_SRC})
endif()

# ---- 主库 ----------------------------------------------------------------
add_library(zh_engine SHARED ${ZH_SOURCES})

target_include_directories(zh_engine
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/src/common
            ${CMAKE_CURRENT_SOURCE_DIR}/data)

target_compile_definitions(zh_engine PRIVATE _GNU_SOURCE)
if(ZH_PORTABLE)
    target_compile_definitions(zh_engine PRIVATE ZH_PORTABLE=1)
    if(ANDROID)
        # NDK r27 默认 _FORTIFY_SOURCE=2，会引入 __*_chk 系列 bionic 专有符号
        target_compile_options(zh_engine PRIVATE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
        target_link_options(zh_engine PRIVATE -Wl,--allow-shlib-undefined)
    endif()
endif()

# ---- 全局链接选项（所有目标继承）-------------------------------------
# Android 15+ 支持 16KB 页；不声明 max-page-size 会让 .so 在某些设备上加载失败。
set(ZH_ANDROID_LINK_FLAGS "")
if(ANDROID)
    set(ZH_ANDROID_LINK_FLAGS "-Wl,-z,max-page-size=16384")
    add_link_options(-Wl,-z,max-page-size=16384)
endif()

# ---- 编译选项 ------------------------------------------------------------
if(ANDROID)
    # Android：不链接 libc++，只用 bionic
    target_compile_options(zh_engine PRIVATE -Os -ffunction-sections -fdata-sections)
    target_link_options(zh_engine PRIVATE
        # Android 15+ 支持 16KB 页，必须声明，否则新设备上崩溃
        -Wl,-z,max-page-size=16384
        -Wl,--gc-sections
        -Wl,--exclude-libs,ALL
        -Wl,--build-id=none)
else()
    target_compile_options(zh_engine PRIVATE -O2)
endif()

# ---- 导出符号控制（所有平台生效）-------------------------------------
# 只导出公开 API，隐藏内部实现。作用：
#   1. 防止其他 .so 意外链接到内部函数
#   2. 避免多探针并存时的符号冲突
#   3. 减小 .dynsym 体积
set(ZH_MAP_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/zh_engine.map.txt)
if(EXISTS ${ZH_MAP_FILE})
    if(APPLE)
        target_link_options(zh_engine PRIVATE
            -Wl,-exported_symbols_list,${ZH_MAP_FILE})
    else()
        target_link_options(zh_engine PRIVATE
            -Wl,-soname,libzh_engine.so
            -Wl,--version-script=${ZH_MAP_FILE})
    endif()
    message(STATUS "zh_engine: 使用导出符号表 ${ZH_MAP_FILE}")
else()
    message(WARNING "zh_engine: 未找到 ${ZH_MAP_FILE}，所有符号将导出")
endif()

if(ZH_STRICT)
    target_compile_options(zh_engine PRIVATE
        -Wall -Wextra
        -Wno-unused-parameter
        -Wno-sign-compare
        # 数据表用 {a, b} 只填前几个字段，其余自动为 0 —— 这是有意为之，
        # 124 条表全部如此。关掉这条警告，其余保持开启。
        -Wno-missing-field-initializers)
endif()

if(ZH_WERROR)
    target_compile_options(zh_engine PRIVATE -Werror)
endif()

if(ZH_USE_ASAN)
    target_compile_options(zh_engine PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(zh_engine PRIVATE -fsanitize=address)
endif()

# ---- 静态库（可选）------------------------------------------------------
if(ZH_BUILD_STATIC)
    add_library(zh_engine_static STATIC ${ZH_SOURCES})
    target_include_directories(zh_engine_static
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                ${CMAKE_CURRENT_SOURCE_DIR}/src/common
                ${CMAKE_CURRENT_SOURCE_DIR}/data)
    target_compile_definitions(zh_engine_static PRIVATE _GNU_SOURCE)
    # 需要 PIC 才能被链进 .so（诊断探针会 whole-archive 整个引擎）
    set_target_properties(zh_engine_static PROPERTIES
        OUTPUT_NAME zh_engine
        POSITION_INDEPENDENT_CODE ON)
endif()

# ---- 测试 ----------------------------------------------------------------
if(ZH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---- 示例 ----------------------------------------------------------------
if(ZH_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ---- 安装 ----------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS zh_engine
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${ZH_PUBLIC_HEADERS} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zh)

# ---- 摘要 ----------------------------------------------------------------
message(STATUS "")
message(STATUS "=================================================")
message(STATUS " ZhaoHua Engine ${PROJECT_VERSION}")
message(STATUS "   arch        : ${ZH_ARCH}")
message(STATUS "   tests       : ${ZH_BUILD_TESTS}")
message(STATUS "   examples    : ${ZH_BUILD_EXAMPLES}")
message(STATUS "   static lib  : ${ZH_BUILD_STATIC}")
message(STATUS "   asan        : ${ZH_USE_ASAN}")
message(STATUS "=================================================")
message(STATUS "")
