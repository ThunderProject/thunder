#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <vector>
#include <atomic>
#include <latch>
#include <chrono>
#include <type_traits>

import spinlock;

using namespace thunder;

TEST_CASE("try_lock succeeds when free, fails when held", "[spinlock]") {
    spinlock lock;

    REQUIRE(lock.try_lock());
    REQUIRE_FALSE(lock.try_lock());
    lock.unlock();

    REQUIRE(lock.try_lock());
    lock.unlock();
}

TEST_CASE("lock/unlock provide mutual exclusion for increments", "[spinlock]") {
    constexpr int32_t numThreads = 8;
    constexpr int32_t iters = 50000;

    int32_t counter = 0;

    {
        spinlock lock;
        std::latch start_gate(numThreads);
        std::vector<std::jthread> threads;
        threads.reserve(numThreads);

        for (int i = 0; i < numThreads; i++) {
            threads.emplace_back([&] {
                start_gate.count_down();
                start_gate.wait();

                for (int j = 0; j < iters; j++) {
                    lock.lock();
                    counter += 1;
                    lock.unlock();
                }
            });
        }
    }

    REQUIRE(counter == numThreads * iters);
}

TEST_CASE("non-recursive semantics: try_lock fails if already held by the same thread", "[spinlock]") {
    spinlock lock;

    lock.lock();
    REQUIRE_FALSE(lock.try_lock());
    lock.unlock();

    REQUIRE(lock.try_lock());
    lock.unlock();
}

TEST_CASE("try_lock is non-blocking while another thread holds the lock", "[spinlock]") {
    using namespace std::chrono_literals;
    spinlock lock;

    std::atomic ready{false};
    std::thread holder([&] {
        lock.lock();
        ready.store(true, std::memory_order_release);
        std::this_thread::sleep_for(50ms);
        lock.unlock();
    });

    while (!ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    REQUIRE_FALSE(lock.try_lock());

    holder.join();
}

TEST_CASE("test backoff policy", "[spinlock]") {
    constexpr int32_t numThreads = 8;
    constexpr int32_t iters = 50000;

    int32_t counter = 0;

    {
        spinlock lock;
        std::latch start_gate(numThreads);
        std::vector<std::jthread> threads;
        threads.reserve(numThreads);

        for (int i = 0; i < numThreads; i++) {
            threads.emplace_back([&] {
                start_gate.count_down();
                start_gate.wait();

                for (int j = 0; j < iters; j++) {
                    lock.lock();
                    counter += 1;
                    lock.unlock();
                }
            });
        }
    }

    REQUIRE(counter == numThreads * iters);
}