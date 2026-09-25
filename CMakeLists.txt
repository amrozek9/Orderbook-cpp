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

# Differential-testing harness: a naive reference book, a biased sequence
# generator, and a shrinker. Shared by the test suite and the fuzz driver.
add_library(difftest_harness tests/diff_harness.cpp)
target_include_directories(difftest_harness PUBLIC tests)
target_link_libraries(difftest_harness PUBLIC lob)
target_compile_options(difftest_harness PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_executable(tests
    tests/test_order_book.cpp
    tests/test_differential.cpp)
target_compile_options(tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(tests PRIVATE lob difftest_harness Catch2::Catch2WithMain)

# Long-running fuzzer: ./difftest [sequences] [first_seed] [ops_per_sequence]
add_executable(difftest tests/difftest_main.cpp)
target_compile_options(difftest PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(difftest PRIVATE lob difftest_harness)
include(Catch)
catch_discover_tests(tests)
