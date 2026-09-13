cmake_minimum_required(VERSION 3.18)
project(SCFConsumer LANGUAGES CXX)

find_package(SCF REQUIRED)

add_executable(app main.cpp)
# 无命名空间 —— 直接以 scf 链接
target_link_libraries(app scf)

# 必须调用：加 Common-Controls v6 + PerMonitorV2 manifest 并设置 C++20。
# 少了这一步控件会回退到 XP 外观（没有主题、DPI 也不对）。
scf_setup_executable(app)
