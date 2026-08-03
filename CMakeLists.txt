cmake_minimum_required(VERSION 3.10)
project(simpleCpp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The source file name contains '+' characters, so quote it explicitly.
add_executable(nano_cc "simpleC++.c")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nano_cc PRIVATE -Wall -Wextra)
endif()
