#include <catch2/catch_test_macros.hpp>
#include <string>
#include <thread>

import mpmc_queue;

using namespace thunder;

TEST_CASE("queue basic push/pop", "[mpmc_queue]") {
    mpmc::queue<int32_t> queue(4);

    SECTION("push and pop lvalue") {
        constexpr auto item = 42;
        queue.push(item);
        auto item2 = queue.pop();
        REQUIRE(item2 == 42);
    }

    SECTION("push and pop rvalue") {
        queue.push(123);
        auto item2 = queue.pop();
        REQUIRE(item2 == 123);
    }

    SECTION("emplace") {
        queue.emplace(5);
        auto item2 = queue.pop();
        REQUIRE(item2 == 5);
    }
}

TEST_CASE("queue basic push/try_pop", "[mpmc_queue]") {
    mpmc::queue<int32_t> queue(4);

    SECTION("push and try_pop lvalue") {
        constexpr auto item = 42;
        queue.push(item);

        auto item2 = queue.try_pop();
        REQUIRE(item2.has_value());
        REQUIRE(item2.value() == 42);
    }

    SECTION("push and try_pop rvalue") {
        queue.push(123);

        auto item2 = queue.try_pop();
        REQUIRE(item2.has_value());
        REQUIRE(item2.value() == 123);
    }

    SECTION("emplace") {
        queue.emplace(5);
        auto item2 = queue.try_pop();
        REQUIRE(item2.has_value());
        REQUIRE(item2.value() == 5);
    }
}

TEST_CASE("queue basic try_push", "[mpmc_queue]") {
    mpmc::queue<int32_t> queue(4);

    SECTION("try_push and try_pop lvalue") {
        constexpr auto item = 42;
        REQUIRE(queue.try_push(item));

        auto item2 = queue.try_pop();
        REQUIRE(item2.has_value());
        REQUIRE(item2.value() == 42);
    }

    SECTION("try_push and try_pop rvalue") {
        REQUIRE(queue.try_push(123));

        auto item2 = queue.try_pop();
        REQUIRE(item2.has_value());
        REQUIRE(item2.value() == 123);
    }

    SECTION("try_emplace") {
        REQUIRE(queue.try_emplace(5));
        auto item2 = queue.try_pop();
        REQUIRE(item2.has_value());
        REQUIRE(item2.value() == 5);
    }
}