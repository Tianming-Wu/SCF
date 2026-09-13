cmake_minimum_required(VERSION 3.18)
project(SCFConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

find_package(SCF REQUIRED)

add_executable(app main.cpp)
# 无命名空间 —— 直接以 scf 链接
target_link_libraries(app scf)
