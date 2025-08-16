#include <catch2/catch_test_macros.hpp>

import backoff;
using namespace thunder;

TEST_CASE("backoff starts not completed", "[backoff]") {
    backoff backoff;
    REQUIRE_FALSE(backoff.is_completed());
}

TEST_CASE("spin alone does not complete the backoff", "[backoff]") {
    backoff backoff;

    for (int i = 0; i < 10000; i++) {
        backoff.spin();
    }
    REQUIRE_FALSE(backoff.is_completed());
}

TEST_CASE("snooze completes after exceeding yield limit", "[backoff]") {
    backoff backoff;

    // With the current implementation (yield limit = 10), the backoff becomes
    // completed on the 11th snooze from a fresh state.
    for (int i = 0; i < 10; i++) {
        backoff.snooze();
        REQUIRE_FALSE(backoff.is_completed()); // not completed yet
    }

    backoff.snooze(); // 11th call
    REQUIRE(backoff.is_completed());
}

TEST_CASE("reset returns to initial state", "[backoff]") {
    backoff backoff;

    // Drive it to completion…
    for (int i = 0; i < 11; ++i) {
        backoff.snooze();
    }
    REQUIRE(backoff.is_completed());

    // reset and verify it's usable again.
    backoff.reset();
    REQUIRE_FALSE(backoff.is_completed());

    for (int i = 0; i < 10; i++) {
        backoff.snooze();
        REQUIRE_FALSE(backoff.is_completed());
    }
    backoff.snooze();
    REQUIRE(backoff.is_completed());
}

TEST_CASE("spins followed by snoozes eventually complete", "[backoff]") {
    backoff backoff;

    for (int i = 0; i < 10000; i++) {
        backoff.spin();
    }
    REQUIRE_FALSE(backoff.is_completed());

    // snooze should eventually push it over the yield limit.
    bool completed = false;
    for (int i = 0; i < 11 && !completed; i++) {
        backoff.snooze();
        completed = backoff.is_completed();
    }
    REQUIRE(completed);
}