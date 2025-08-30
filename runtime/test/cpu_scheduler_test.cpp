#define CATCH_CONFIG_MAIN
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <latch>
#include <optional>
#include <thread>
#include <vector>
#include <numeric>
#include <coroutine>

import cpu_scheduler;
import coro;
using namespace thunder;

using namespace std::chrono_literals;

static std::size_t thread_id_hash() {
    return std::hash<std::thread::id>{}(std::this_thread::get_id());
}

auto& scheduler = cpu::scheduler::global();

TEST_CASE("cpu_scheduler executes tasks and returns results", "[cpu_scheduler]") {
    constexpr int numComputations = 200;
    std::vector<std::future<int>> futures;
    futures.reserve(numComputations);

    for (int i = 0; i < numComputations; i++) {
        futures.emplace_back(scheduler.submit([i] { return i * i; }));
    }

    long long sum = 0;
    for (auto& future : futures) {
        REQUIRE(future.wait_for(5s) == std::future_status::ready);
        sum += future.get();
    }

    constexpr long long expected = static_cast<long long>(numComputations - 1) * numComputations * (2LL * numComputations - 1) / 6LL;
    REQUIRE(sum == expected);
}

TEST_CASE("cpu_scheduler handles mixed workloads and work stealing", "[scheduler]") {
    constexpr int heavyTasks = 50;
    constexpr int lightTasks = 1000;
    std::atomic completed{0};

    for (int i = 0; i < heavyTasks; i++) {
        scheduler.submit([&completed, i] {
            volatile std::uint64_t x = 0;
            for (int k = 0; k < 50000 + (i % 1000); k++) {
                x += (k ^ i);
            }

            (void)x;
            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    for (int i = 0; i < lightTasks; i++) {
        scheduler.submit([&completed] { completed.fetch_add(1, std::memory_order_relaxed); });
    }

    constexpr auto target = heavyTasks + lightTasks;

    const auto start = std::chrono::steady_clock::now();
    while (completed.load(std::memory_order_relaxed) != target) {
        std::this_thread::sleep_for(2ms);
        REQUIRE(std::chrono::steady_clock::now() - start < 10s);
    }

    REQUIRE(completed.load() == target);
}

TEST_CASE("cpu_scheduler wakes parked threads for new work", "[scheduler]") {
    // Let workers run and likely go idle/park.
    std::this_thread::sleep_for(500ms);

    std::promise<void> promise;
    auto future = promise.get_future();

    scheduler.submit([&promise] {
        promise.set_value();
    });

    REQUIRE(future.wait_for(5s) == std::future_status::ready);
}

TEST_CASE("cpu_scheduler coroutines can be scheduled to continue execution on the scheduler", "[scheduler]") {
    std::promise<void> donePromise;
    auto doneFuture = donePromise.get_future();

    std::atomic<std::size_t> beforeId{0};
    std::atomic<std::size_t> afterId{0};
    std::atomic<std::size_t> after2Id{0};
    std::atomic<std::size_t> after3Id{0};
    std::atomic<std::size_t> after4Id{0};

    thunder::coro::spawn([&donePromise, &beforeId, &afterId, &after2Id, &after3Id, &after4Id]() -> coro::task<> {
        beforeId.store(thread_id_hash(), std::memory_order_relaxed);

        // The first sched should move from the test thread to a worker thread in the threadpool
        co_await scheduler.sched();

        // The rest of the "sched" may or amy not switch thread. Depends on if the work got stolen or not
        afterId.store(thread_id_hash(), std::memory_order_relaxed);
        co_await scheduler.sched();

        after2Id.store(thread_id_hash(), std::memory_order_relaxed);
        co_await scheduler.sched();

        after3Id.store(thread_id_hash(), std::memory_order_relaxed);
        co_await scheduler.sched();

        after4Id.store(thread_id_hash(), std::memory_order_relaxed);
        donePromise.set_value();

        co_return;
   }());

    REQUIRE(doneFuture.wait_for(5s) == std::future_status::ready);

    const auto before = beforeId.load(std::memory_order_relaxed);
    const auto after1 = afterId.load(std::memory_order_relaxed);
    const auto after2 = after2Id.load(std::memory_order_relaxed);
    const auto after3 = after3Id.load(std::memory_order_relaxed);
    const auto after4 = after4Id.load(std::memory_order_relaxed);

    REQUIRE(before != 0);
    REQUIRE(after1 != 0);
    REQUIRE(after2 != 0);
    REQUIRE(after3 != 0);
    REQUIRE(after4 != 0);


    // Before should not be the same id as after1 because we started on a thread not owned by the scheduler,
    // but for the other "after" variables, they may all be the same id. Because now we are on a thread owned
    // by the scheduler, so the task will get pushed to the worker thread's local queue. We will only get a new/different
    // id for the other "after" variables if a task was stolen by another thread, and we can't really know if that happens
    // or not from the outside
    REQUIRE(before != after1);
}

TEST_CASE("cpu_scheduler many coroutines can reschedule and complete", "[scheduler]") {
    constexpr auto numCoroutines = 200;
    std::latch done(numCoroutines);
    std::atomic hops{0};

    for (int i = 0; i < numCoroutines; i++) {
        thunder::coro::spawn([&done, &hops]() -> coro::task<> {
            co_await scheduler.sched();
            hops.fetch_add(1, std::memory_order_relaxed);
            co_await scheduler.sched();
            hops.fetch_add(1, std::memory_order_relaxed);
            done.count_down();
            co_return;
        }());
    }

    // Wait for all coroutines to finish.
    // If they don't, we'll time out the test.
    const auto start = std::chrono::steady_clock::now();
    while (done.try_wait() == false) {
        std::this_thread::sleep_for(2ms);
        REQUIRE(std::chrono::steady_clock::now() - start < 10s);
    }

    // Each coroutine hopped twice.
    REQUIRE(hops.load() == numCoroutines * 2);
}

TEST_CASE("cpu_scheduler basic scheduler throughput", "[scheduler]") {
    std::atomic counter{0};

    for (int i = 0; i < 5000; i++) {
        scheduler.submit([&counter] {
            volatile std::uint64_t x = 0;
            for (int k = 0; k < 2000; ++k) {
                x += k;
            }
            (void)x;
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }

    for (int i = 0; i < 5000; i++) {
        scheduler.submit([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }

    constexpr int target = 10000;
    const auto start = std::chrono::steady_clock::now();

    while (counter.load(std::memory_order_relaxed) != target) {
        std::this_thread::sleep_for(2ms);
        REQUIRE(std::chrono::steady_clock::now() - start < 10s);
    }

    REQUIRE(counter.load() == target);
}
