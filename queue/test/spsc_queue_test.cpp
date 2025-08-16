#include <catch2/catch_test_macros.hpp>
#include <string>
#include <thread>

import spsc_queue;

using namespace thunder;

TEST_CASE("queue basic push/pop", "[queue]") {
    spsc::queue<int32_t> queue(4);

    SECTION("push and pop lvalue") {
        constexpr auto item = 42;
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

struct MoveOnly {
    int v{0};
    MoveOnly() = default;
    explicit MoveOnly(int x) : v(x) {}
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    MoveOnly(MoveOnly&&) noexcept = default;
    MoveOnly& operator=(MoveOnly&&) noexcept = default;
    bool operator==(const MoveOnly& o) const { return v == o.v; }
};

struct Point {
    int x{}, y{};
    Point() = default;
    Point(int xx, int yy) : x(xx), y(yy) {}
    bool operator==(const Point& o) const { return x == o.x && y == o.y; }
};

TEST_CASE("capacity() reports logical capacity", "[capacity]") {
    const spsc::queue<int32_t> queue(8);
    REQUIRE(queue.capacity() == 8);

    spsc::queue<int32_t> queue2(1);
    REQUIRE(queue2.capacity() == 1);
}

TEST_CASE("empty()/size() initial state", "[size][empty]") {
    const spsc::queue<int32_t> queue(4);
    REQUIRE(queue.capacity() == 4);
    REQUIRE(queue.empty());
    REQUIRE(queue.size() == 0);
}


TEST_CASE("empty()/size() after pushes and pops", "[size][empty]") {
    spsc::queue<int32_t> queue(4);

    // push up to full
    REQUIRE(queue.empty());
    REQUIRE(queue.size() == 0);

    REQUIRE(queue.try_push(1));
    REQUIRE_FALSE(queue.empty());
    REQUIRE(queue.size() == 1);

    REQUIRE(queue.try_push(2));
    REQUIRE(queue.size() == 2);

    REQUIRE(queue.try_push(3));
    REQUIRE(queue.size() == 3);

    REQUIRE(queue.try_push(4));
    REQUIRE(queue.size() == 4);

    // Next push should fail (logical capacity = 4)
    REQUIRE_FALSE(queue.try_push(5));
    REQUIRE(queue.size() == 4);

    // pop one, size drops
    auto v = queue.try_pop();
    REQUIRE(v.has_value());
    REQUIRE(v.value() == 1);
    REQUIRE(queue.size() == 3);
    REQUIRE_FALSE(queue.empty());

    // push succeeds again
    REQUIRE(queue.try_push(4));
    REQUIRE(queue.size() == 4);
}

TEST_CASE("wrap-around preserves size()/empty()", "[wrap][size][empty]") {
    // Small capacity to force wrap quickly
    spsc::queue<int32_t> queue(3);

    // Fill to max resident (which is capacity)
    REQUIRE(queue.try_push(10));
    REQUIRE(queue.try_push(20));
    REQUIRE(queue.try_push(30));
    REQUIRE(queue.size() == 3);
    REQUIRE_FALSE(queue.try_push(40)); // full

    // Pop one and push one to wrap
    REQUIRE(queue.try_pop().value() == 10);
    REQUIRE(queue.size() == 2);

    REQUIRE(queue.try_push(40)); // should wrap internally
    REQUIRE(queue.size() == 3);
    REQUIRE_FALSE(queue.empty());

    // Drain and check empty at the end
    REQUIRE(queue.try_pop().value() == 20);
    REQUIRE(queue.try_pop().value() == 30);
    REQUIRE(queue.try_pop().value() == 40);
    REQUIRE(queue.empty());
    REQUIRE(queue.size() == 0);
}