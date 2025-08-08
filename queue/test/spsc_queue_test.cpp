#include <catch2/catch_test_macros.hpp>
#include <string>
#include "../spsc_queue.h"

#include <thread>

using namespace thunder;

TEST_CASE("queue basic push/pop", "[queue]") {
    spsc::queue<int32_t> queue(4);

    SECTION("push and pop lvalue") {
        const auto item = 42;
        queue.push(item);
        const auto out = queue.pop();
        REQUIRE(out == 42);
    }

    SECTION("push and pop rvalue") {
        queue.push(123);
        const auto out = queue.pop();
        REQUIRE(out == 123);
    }

    SECTION("emplace") {
        queue.emplace(5);
        REQUIRE(queue.pop() == 5);
    }
}

TEST_CASE("queue try_push/try_pop", "[queue]") {
    spsc::queue<std::string> queue(2);

    REQUIRE(queue.try_push("first"));
    REQUIRE(queue.try_push("second"));

    // should fail when full
    REQUIRE_FALSE(queue.try_push("third"));

    auto v1 = queue.try_pop();
    REQUIRE(v1.has_value());
    REQUIRE(*v1 == "first");

    std::string out = queue.try_pop().value_or("oops");
    REQUIRE(out == "second");

    //now empty
    REQUIRE_FALSE(queue.try_pop());
}

TEST_CASE("SPSC producer/consumer threaded test", "[queue][threads]") {
    constexpr std::size_t N = 100'000; // total items to send
    spsc::queue<std::size_t> queue(1024);

    std::vector<std::size_t> consumed;
    consumed.reserve(N);

    std::thread producer([&] {
        for (std::size_t i = 0; i < N; ++i) {
            while (!queue.try_push(i)) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&] {
        for (std::size_t i = 0; i < N; ++i) {
            std::optional<std::size_t> val;
            do {
                val = queue.try_pop();
                if (!val) {
                    std::this_thread::yield();
                }
            } while (!val);
            consumed.push_back(*val);
        }
    });

    producer.join();
    consumer.join();

    // Verify we got exactly sequence 0..N-1 in order
    REQUIRE(consumed.size() == N);
    for (std::size_t i = 0; i < N; ++i) {
        REQUIRE(consumed[i] == i);
    }
}