#include <catch2/catch_test_macros.hpp>
#include <string>
#include "../spsc_queue.h"

#include <thread>

using namespace thunder;

//heap_buffer tests
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

//stack_buffer tests
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

TEST_CASE("stack queue basic push/pop", "[stack][basic]") {
    spsc::queue<int32_t, 8> queue(0);

    SECTION("lvalue push/pop") {
        constexpr int value = 42;
        queue.push(value);
        REQUIRE(queue.pop() == 42);
    }

    SECTION("rvalue push/pop") {
        queue.push(123);
        REQUIRE(queue.pop() == 123);
    }

    SECTION("emplace") {
        queue.emplace(5);
        REQUIRE(queue.pop() == 5);
    }
}

TEST_CASE("stack queue try_push/try_pop", "[stack]") {
    spsc::queue<std::string, 3> queue(0);

    REQUIRE(queue.try_push(std::string("first")));
    REQUIRE(queue.try_push(std::string("second")));
    REQUIRE(queue.try_push(std::string("third")));
    REQUIRE_FALSE(queue.try_push(std::string("fourth"))); // queue full

    auto v1 = queue.try_pop();
    REQUIRE(v1.has_value());
    REQUIRE(*v1 == "first");

    std::string out = queue.try_pop().value_or("oops");
    REQUIRE(out == "second");

    auto v3 = queue.try_pop();
    REQUIRE(v3.has_value());
    REQUIRE(v3.value() == "third");

    // now empty
    REQUIRE_FALSE(queue.try_pop().has_value());
}

TEST_CASE("stack queue works with move-only types", "[stack][move]") {
    spsc::queue<MoveOnly, 2> queue(0);

    MoveOnly m(7);
    queue.push(std::move(m));
    const auto out = queue.pop();
    REQUIRE(out.v == 7);

    // try_push rvalue path
    REQUIRE(queue.try_push(MoveOnly{9}));
    REQUIRE(queue.pop().v == 9);
}

TEST_CASE("stack queue emplace with composite type", "[stack][emplace]") {
    spsc::queue<Point, 4> queue(0);
    queue.emplace(1, 2);
    queue.emplace(3, 4);
    REQUIRE(queue.pop() == Point{1, 2});
    REQUIRE(queue.pop() == Point{3, 4});
}

TEST_CASE("SPSC producer/consumer threaded test (stack)", "[stack][threads][spsc]") {
    constexpr std::size_t N = 100'000; // total items to send
    spsc::queue<std::size_t, 1024> queue(0);

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