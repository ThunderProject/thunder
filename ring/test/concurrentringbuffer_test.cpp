#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

import concurrent_ringbuffer;

TEST_CASE("concurrentringbuffer basic capacity", "[ringbuffer]") {
    constexpr uint64_t requested = 10;
    const thunder::concurrentringbuffer<int32_t> buffer(requested);

    REQUIRE(buffer.capacity() == 16);  // next power of 2
}

TEST_CASE("concurrentringbuffer write/read", "[ringbuffer]") {
    thunder::concurrentringbuffer<int32_t> buffer(8);

    buffer.write_at(0, 42);
    buffer.write_at(1, 99);

    REQUIRE(buffer.read_at(0) == 42);
    REQUIRE(buffer.read_at(1) == 99);
    REQUIRE(buffer.read_at(2) == 0);
    REQUIRE(buffer[0] == 42);
}

TEST_CASE("concurrentringbuffer index wrapping", "[ringbuffer]") {
    thunder::concurrentringbuffer<int32_t> buffer(4);

    buffer.write_at(0, 1);
    buffer.write_at(4, 2);  // same slot as index 0 due to wrapping

    REQUIRE(buffer.read_at(0) == 2);  // overwritten
    REQUIRE(buffer.read_at(4) == 2);
}

TEST_CASE("concurrentringbuffer resize", "[ringbuffer]") {
    thunder::concurrentringbuffer<int32_t> buffer(4);

    for (int i = 0; i < 4; ++i) {
        buffer.write_at(i, i * 10);
    }

    const auto resized = buffer.resize(4, 0);
    REQUIRE(resized.has_value());

    const auto newBuffer = resized.value();
    REQUIRE(newBuffer->capacity() == 8);

    for (int i = 0; i < 4; ++i) {
        REQUIRE(newBuffer->read_at(i) == i * 10);
    }

    delete newBuffer;
}