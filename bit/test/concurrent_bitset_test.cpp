#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <thread>
#include <barrier>
#include <vector>

import concurrent_bitset;

using namespace thunder;

TEST_CASE("concurrent_bitset basic set and test", "[bitset]") {
    concurrent_bitset<64> bitset;

    REQUIRE_FALSE(bitset[0]);
    REQUIRE_FALSE(bitset.test(1, std::memory_order_relaxed));

    REQUIRE_FALSE(bitset.set(0, std::memory_order_relaxed));
    REQUIRE(bitset[0]);
    REQUIRE(bitset.set(0, std::memory_order_relaxed)); // Already set

    REQUIRE_FALSE(bitset.set(1, false, std::memory_order_relaxed)); // Was already 0
    REQUIRE_FALSE(bitset[1]);
    REQUIRE_FALSE(bitset.set(1, true, std::memory_order_relaxed));  // Was 0
    REQUIRE(bitset[1]);
}

TEST_CASE("concurrent_bitset reset functionality", "[bitset]") {
    concurrent_bitset<32> bitset;

    bitset.set(5, std::memory_order_relaxed);
    REQUIRE(bitset[5]);

    REQUIRE(bitset.reset(5, std::memory_order_relaxed)); // was 1
    REQUIRE_FALSE(bitset[5]);
    REQUIRE_FALSE(bitset.reset(5, std::memory_order_relaxed)); // was already 0
}

TEST_CASE("concurrent_bitset get and test equivalence", "[bitset]") {
    concurrent_bitset<32> bitset;
    bitset.set(10, std::memory_order_relaxed);

    REQUIRE(bitset.test(10, std::memory_order_relaxed));
    REQUIRE(bitset.get(10, std::memory_order_relaxed));
}

TEST_CASE("concurrent_bitset edge bit indices", "[bitset]") {
    constexpr size_t size = 128;
    concurrent_bitset<size> bitset;

    REQUIRE_FALSE(bitset[size - 1]);
    bitset.set(size - 1, std::memory_order_relaxed);
    REQUIRE(bitset[size - 1]);

    bitset.reset(size - 1, std::memory_order_relaxed);
    REQUIRE_FALSE(bitset[size - 1]);

    bitset.set(0, std::memory_order_relaxed);
    REQUIRE(bitset[0]);
}

TEST_CASE("concurrent_bitset does not affect other bits", "[bitset][isolation]") {
    concurrent_bitset<64> bitset;

    bitset.set(2, std::memory_order_relaxed);
    REQUIRE(bitset[2]);

    for (size_t i = 0; i < 64; ++i) {
        if (i != 2) {
            REQUIRE_FALSE(bitset[i]);
        }
    }
}

TEST_CASE("concurrent_bitset concurrent set and test", "[bitset][concurrent]") {
    constexpr size_t num_threads = 8;
    constexpr size_t size = 256;
    constexpr size_t bits_per_thread = size / num_threads;

    concurrent_bitset<size> bitset;
    std::barrier sync(num_threads);
    std::vector<std::jthread> threads;

    for (size_t t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t] {
            sync.arrive_and_wait();
            for (size_t i = 0; i < bits_per_thread; ++i) {
                size_t idx = t * bits_per_thread + i;
                bitset.set(idx, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    for (size_t i = 0; i < size; ++i) {
        REQUIRE(bitset[i]);
    }
}
