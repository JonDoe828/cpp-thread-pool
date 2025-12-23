cmake_minimum_required(VERSION 3.16)

project(cpp_thread_pool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- library ----
add_library(thread_pool
    src/thread_pool.cpp
)

target_include_directories(thread_pool
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(thread_pool PUBLIC pthread)

# ---- demo ----
add_executable(demo
    src/main.cpp
)

target_link_libraries(demo PRIVATE thread_pool)

# ---- tests (Catch2) ----
include(CTest)
enable_testing()

include(FetchContent)
FetchContent_Declare(
  Catch2
  URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.5.4.zip
)
FetchContent_MakeAvailable(Catch2)

add_executable(thread_pool_tests
    src/thread_pool.test.cpp
)

target_link_libraries(thread_pool_tests
    PRIVATE thread_pool Catch2::Catch2WithMain
)

add_test(NAME thread_pool_tests COMMAND thread_pool_tests)