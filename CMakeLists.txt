cmake_minimum_required(VERSION 3.18)

project(SCF
    VERSION 1.0.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

include(GNUInstallDirs)
set(INSTALL_LIBDIR ${CMAKE_INSTALL_LIBDIR})
set(INSTALL_INCLUDEDIR ${CMAKE_INSTALL_INCLUDEDIR}/scf)
set(INSTALL_CMAKEDIR ${CMAKE_INSTALL_LIBDIR}/cmake/SCF)

set(CMAKE_DEBUG_POSTFIX "d")
set(CMAKE_RELEASE_POSTFIX "")
set(CMAKE_RELWITHDEBINFO_POSTFIX "rd")
set(CMAKE_MINSIZEREL_POSTFIX "s")

find_package(SharedCppLib2 REQUIRED)

add_library(scf SHARED
    scf.hpp scf.cpp
    scf_tray.hpp scf_tray.cpp
)

# Windows 导出符号：构建 DLL 时 dllexport（scf.hpp 里由 SCF_CPP 触发），
# 消费方自动 dllimport。与 scf.cpp 顶部的手动 #define SCF_CPP 保持一致。
target_compile_definitions(scf PRIVATE SCF_CPP)

# 依赖以 PUBLIC 传递，下游 find_package(SCF) 后自动获得 SharedCppLib2 头文件/库
target_link_libraries(scf
    PUBLIC
        SharedCppLib2::basic
        SharedCppLib2::types
        SharedCppLib2::bitmap
    PRIVATE
        # platform.hpp 的错误翻译只在库内部使用，不暴露给下游
        SharedCppLib2::platform
        # ComCtl32（commctrl.h / InitCommonControlsEx 等）
        comctl32
        # Shell32（托盘图标 Shell_NotifyIcon）
        shell32
)

if(MSVC)
    target_compile_options(scf PRIVATE /source-charset:utf-8 /execution-charset:utf-8)
endif()

# ---- 静态库变体：构建与消费方均定义 SCF_STATIC，使 SCF_EXPORT 为空 ----
add_library(scf_static STATIC
    scf.hpp scf.cpp
    scf_tray.hpp scf_tray.cpp
)
target_compile_definitions(scf_static PUBLIC SCF_STATIC)
target_link_libraries(scf_static
    PUBLIC
        SharedCppLib2::basic
        SharedCppLib2::types
        SharedCppLib2::bitmap
    PRIVATE
        # platform.hpp 的错误翻译只在库内部使用，不暴露给下游
        SharedCppLib2::platform
        # ComCtl32（commctrl.h / InitCommonControlsEx 等）
        comctl32
        # Shell32（托盘图标 Shell_NotifyIcon）
        shell32
)

if(MSVC)
    target_compile_options(scf_static PRIVATE /source-charset:utf-8 /execution-charset:utf-8)
endif()

# ---- scfcli：命令行前端（后续让部分功能可从终端调用） ----
add_executable(scfcli scfcli.cpp scfhelp.cpp)
# fileio：scfcli 直接调用 scl2::readFile（scf 库本身不依赖它）
target_link_libraries(scfcli PRIVATE scf SharedCppLib2::fileio)
if(MSVC)
    # UTF-8 源码 + 执行字符集（与 scf / scf_static 一致），避免字面量/窄字符串乱码
    target_compile_options(scfcli PRIVATE /source-charset:utf-8 /execution-charset:utf-8)
    # 内嵌应用 manifest（comctl v6 + PerMonitorV2），与安装包给下游消费方的一致
    target_link_options(scfcli PRIVATE "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/cmake/scf_app.manifest")
endif()

# ---- 安装 ----

# 头文件
install(FILES scf.hpp scf_tray.hpp DESTINATION ${INSTALL_INCLUDEDIR})

# 库 + 导出目标
install(TARGETS scf
    EXPORT SCFTargets
    ARCHIVE DESTINATION ${INSTALL_LIBDIR}
    LIBRARY DESTINATION ${INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${INSTALL_INCLUDEDIR}
)

# 导出 targets（不带 NAMESPACE —— 用户直接以 scf 链接）
install(EXPORT SCFTargets
    FILE SCFTargets.cmake
    DESTINATION ${INSTALL_CMAKEDIR}
)

# Config 文件 + 版本文件
include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/SCFConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/SCFConfig.cmake
    INSTALL_DESTINATION ${INSTALL_CMAKEDIR}
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/SCFConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/SCFConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/SCFConfigVersion.cmake
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/scf_app.manifest
    DESTINATION ${INSTALL_CMAKEDIR}
)