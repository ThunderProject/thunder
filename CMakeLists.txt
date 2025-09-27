cmake_minimum_required(VERSION 3.31)

project(thunder)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options(
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/GL>
            $<$<CONFIG:Release>:/Gw>
            $<$<CONFIG:Release>:/Gy>
            $<$<CONFIG:Release>:/Oi>
            $<$<CONFIG:Release>:/arch:AVX2>
            $<$<CONFIG:Release>:/Zc:__cplusplus>
            $<$<CONFIG:Release>:/DNDEBUG>
    )
    add_link_options(
            $<$<CONFIG:Release>:/LTCG>
            $<$<CONFIG:Release>:/OPT:ICF>
            $<$<CONFIG:Release>:/OPT:REF>
    )
endif()

enable_testing()

add_library(thunder STATIC)

target_sources(thunder
        PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
        backoff/backoff.ixx
        queue/spsc_queue.ixx
        queue/mpmc_queue.ixx
        parker/thread_parker.ixx
        bit/concurrent_bitset.ixx
        utils/hints.ixx
        ring/concurrentringbuffer.ixx
        deque/concurrentdeque.ixx
        deque/reclaimer.ixx
        sync/spinlock.ixx
        sync/guard.ixx
        runtime/scheduler/cpu_scheduler.ixx
        runtime/scheduler/thread.ixx
        runtime/scheduler/worker.ixx
        runtime/coro/completion_event.ixx
        runtime/coro/sync_wait.ixx
        runtime/coro/task.ixx
        runtime/coro/spawn.ixx
        runtime/coro/resume_guard.ixx
        runtime/coro/coro.ixx
        PRIVATE
        runtime/scheduler/worker.cpp
)

find_package(libassert REQUIRED)

target_include_directories(thunder INTERFACE  ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(thunder PRIVATE libassert::assert)

add_executable(dummy dummy.cpp
        ring/test/concurrentringbuffer_test.cpp
        bit/test/concurrent_bitset_test.cpp
        deque/test/concurrent_deque_test.cpp
        parker/test/thread_parker_test.cpp
        queue/test/spsc_queue_test.cpp
        queue/test/mpmc_queue_test.cpp
        backoff/test/backoff_test.cpp
        sync/test/spinlock_test.cpp
        sync/test/guard_test.cpp
        runtime/test/cpu_scheduler_test.cpp
)

find_package(Catch2 REQUIRED)

target_link_libraries(dummy thunder libassert::assert Catch2::Catch2WithMain)

find_package(benchmark REQUIRED)
add_executable(bench_spsc_queue queue/benchmark/spsc_queue_benchmark.cpp)
target_link_libraries(bench_spsc_queue thunder libassert::assert benchmark::benchmark_main)
target_compile_features(bench_spsc_queue PRIVATE cxx_std_20)

include(CTest)
include(Catch)
catch_discover_tests(dummy)