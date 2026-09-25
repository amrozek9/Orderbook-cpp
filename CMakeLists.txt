cmake_minimum_required(VERSION 3.20)
project(order_book CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Sanitized builds each get their own tree, because ASan and TSan cannot be
# linked into the same binary:
#   cmake -S . -B build-asan -DLOB_SANITIZER=address   (ASan + UBSan)
#   cmake -S . -B build-tsan -DLOB_SANITIZER=thread    (TSan)
# Applied before Catch2 is fetched so it is instrumented too; uninstrumented
# code is the main source of TSan false positives.
set(LOB_SANITIZER "" CACHE STRING "Sanitizer build: empty, address, or thread")
set_property(CACHE LOB_SANITIZER PROPERTY STRINGS "" address thread)
if(LOB_SANITIZER STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
                        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
elseif(LOB_SANITIZER STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
elseif(NOT LOB_SANITIZER STREQUAL "")
    message(FATAL_ERROR "LOB_SANITIZER must be empty, address, or thread")
endif()

find_package(Threads REQUIRED)

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
    tests/test_differential.cpp
    tests/test_threading.cpp)
target_compile_options(tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(tests PRIVATE lob difftest_harness Catch2::Catch2WithMain
                      Threads::Threads)

# Long-running fuzzer: ./difftest [sequences] [first_seed] [ops_per_sequence]
add_executable(difftest tests/difftest_main.cpp)
target_compile_options(difftest PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(difftest PRIVATE lob difftest_harness)
include(Catch)
catch_discover_tests(tests)
