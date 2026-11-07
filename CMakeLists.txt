cmake_minimum_required(VERSION 3.16)
project(custom_memory_allocator LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)

add_library(allocator src/allocator.c)
target_include_directories(allocator PUBLIC include)
target_link_libraries(allocator PUBLIC Threads::Threads)
target_compile_options(allocator PRIVATE -Wall -Wextra -Wpedantic)

add_executable(allocator_tests tests/allocator_tests.c)
target_link_libraries(allocator_tests PRIVATE allocator)
add_executable(benchmark src/benchmark.c)
target_link_libraries(benchmark PRIVATE allocator)

enable_testing()
add_test(NAME allocator_tests COMMAND allocator_tests)
