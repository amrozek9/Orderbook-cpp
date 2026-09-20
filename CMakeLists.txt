cmake_minimum_required(VERSION 3.20)
project(order_book CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(lob src/order_book.cpp)
target_include_directories(lob PUBLIC include)
target_compile_options(lob PRIVATE -Wall -Wextra -Wpedantic)

include(FetchContent)
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.2)
FetchContent_MakeAvailable(Catch2)

enable_testing()
add_executable(tests tests/test_order_book.cpp)
target_link_libraries(tests PRIVATE lob Catch2::Catch2WithMain)
include(Catch)
catch_discover_tests(tests)