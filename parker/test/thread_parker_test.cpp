#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <chrono>
#include "../thread_parker.h" // adjust include

using namespace std::chrono_literals;
using thunder::thread_parker;

static auto elapsed(std::invocable auto&& f) {
    const auto start = std::chrono::steady_clock::now();
    std::forward<decltype(f)>(f)();
    return std::chrono::steady_clock::now() - start;
}

// 1) park + unpark works (blocks, then continues)
TEST_CASE("park blocks until unpark", "[thread_parker]") {
    thread_parker parker;

    std::jthread thread([&]{
        std::this_thread::sleep_for(50ms);
        parker.unpark();
    });

    auto elapsedTime = elapsed([&]{ parker.park(); });
    REQUIRE(elapsedTime >= 40ms); // generous slack for CI timing
}

// 2) unpark followed by park returns immediately
TEST_CASE("unpark then park is immediate", "[thread_parker]") {
    thread_parker parker;

    parker.unpark();
    auto elapsedTime = elapsed([&]{ parker.park(); });
    REQUIRE(elapsedTime < 5ms);
}

// 3) timed park (park_for) works: wakes before timeout when unpark happens
TEST_CASE("park_for wakes before timeout when unparked", "[thread_parker]") {
    thread_parker parker;

    std::jthread thread([&]{
        std::this_thread::sleep_for(30ms);
        parker.unpark();
    });

    auto elapsedTime = elapsed([&]{
        REQUIRE(parker.park_for(200ms) == true);
    });

    REQUIRE(elapsedTime >= 20ms);
    REQUIRE(elapsedTime < 150ms);
}

// 4) unpark followed by timed park returns immediately
TEST_CASE("unpark then park_for is immediate", "[thread_parker]") {
    thread_parker parker;

    parker.unpark();
    auto elapsedTime = elapsed([&]{ REQUIRE(parker.park_for(500ms) == true); });
    REQUIRE(elapsedTime < 5ms);
}

// 5) timed park with duration 0 returns immediately (no token)
TEST_CASE("park_for(0) returns immediately when no token", "[thread_parker]") {
    thread_parker parker;

    auto elapsedTime = elapsed([&]{ REQUIRE(parker.park_for(0ms) == false); });
    REQUIRE(elapsedTime < 5ms);
}

// 6) timed-until (park_until) works: wakes before deadline when unpark happens
TEST_CASE("park_until wakes before deadline when unparked", "[thread_parker]") {
    thread_parker parker;
    const auto deadline = std::chrono::steady_clock::now() + 100ms;

    std::jthread thread([&]{
        std::this_thread::sleep_for(30ms);
        parker.unpark();
    });

    auto elapsedTime = elapsed([&]{
        REQUIRE(parker.park_until(deadline) == true);
    });

    REQUIRE(elapsedTime >= 20ms);
    REQUIRE(elapsedTime< 90ms);
}

// 7) unpark followed by timed-until park returns immediately
TEST_CASE("unpark then park_until is immediate", "[thread_parker]") {
    thread_parker parker;
    const auto deadline = std::chrono::steady_clock::now() + 1s;

    parker.unpark();
    auto elapsedTime = elapsed([&]{ REQUIRE(parker.park_until(deadline) == true); });
    REQUIRE(elapsedTime < 5ms);
}

// 8) timed-until with past timepoint returns immediately (no token)
TEST_CASE("park_until(past) returns immediately when no token", "[thread_parker]") {
    thread_parker parker;
    const auto past = std::chrono::steady_clock::now() - 1ms;

    auto elapsedTime = elapsed([&]{ REQUIRE(parker.park_until(past) == false); });
    REQUIRE(elapsedTime < 5ms);
}
