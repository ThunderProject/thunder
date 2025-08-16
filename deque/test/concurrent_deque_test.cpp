#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <thread>
#include <barrier>

import concurrent_deque;

using namespace thunder;

TEST_CASE("Basic push/pop LIFO behavior", "[concurrent_deque]") {
    concurrent_deque<int, reclamation_technique::bounded> deque(8);

    REQUIRE(deque.empty());

    REQUIRE(deque.push(1));
    REQUIRE(deque.push(2));
    REQUIRE(deque.push(3));

    REQUIRE(deque.size() == 3);
    REQUIRE_FALSE(deque.empty());

    auto item = deque.pop();
    REQUIRE(item);
    REQUIRE(*item == 3);

    item = deque.pop();
    REQUIRE(item);
    REQUIRE(*item == 2);

    item = deque.pop();
    REQUIRE(item);
    REQUIRE(*item == 1);

    item = deque.pop();
    REQUIRE_FALSE(item);
    REQUIRE(item.error() == PopFailureReason::EmptyQueue);
}

TEST_CASE("Basic push/pop FIFO behavior", "[concurrent_deque]") {
    concurrent_deque<int, reclamation_technique::bounded, Flavor::Fifo> deque(8);

    REQUIRE(deque.push(10));
    REQUIRE(deque.push(20));
    REQUIRE(deque.push(30));

    auto item = deque.pop();
    REQUIRE(item);
    REQUIRE(*item == 10);

    item = deque.pop();
    REQUIRE(item);
    REQUIRE(*item == 20);

    item = deque.pop();
    REQUIRE(item);
    REQUIRE(*item == 30);
}

TEST_CASE("Push fails when buffer is full in bounded mode", "[concurrent_deque]") {
    concurrent_deque<int, reclamation_technique::bounded> deque(2);

    REQUIRE(deque.push(1));
    REQUIRE(deque.push(2));
    REQUIRE_FALSE(deque.push(3)); // Should fail since capacity is 2
}

TEST_CASE("Concurrent steals", "[concurrent_deque][multithreaded]") {
    concurrent_deque<int, reclamation_technique::bounded> deque(64);

    // Fill deque with items
    for (int i = 0; i < 50; ++i) {
        REQUIRE(deque.push(i));
    }

    std::atomic total_stolen = 0;

    auto stealer = [&deque, &total_stolen]() {
        while (true) {
            auto result = deque.steal();
            if (result) {
                ++total_stolen;
            } else if (result.error() == StealFailureReason::EmptyQueue) {
                break;
            }
        }
    };

    std::thread t1(stealer);
    std::thread t2(stealer);
    std::thread t3(stealer);

    // Owner pops too
    int popped = 0;
    while (true) {
        auto result = deque.pop();
        if (result) {
            ++popped;
        } else {
            break;
        }
    }

    t1.join();
    t2.join();
    t3.join();

    REQUIRE(total_stolen + popped == 50);
}

TEST_CASE("Deferred reclamation triggers resize and preserves data", "[concurrent_deque][deferred]") {
    // Small capacity to force resize
    concurrent_deque<int> deque(2);

    REQUIRE(deque.capacity() == 2);

    // Push more than initial capacity
    for (int i = 0; i < 10; i++) {
        REQUIRE(deque.push(i)); // Should succeed and trigger resize internally
    }

    // Expect size to reflect 10 elements and capacity to be 16
    REQUIRE(deque.size() == 10);
    REQUIRE(deque.capacity() == 16);

    // Pop all items and check they match
    for (int expected = 9; expected >= 0; expected--) {
        auto result = deque.pop();
        REQUIRE(result);
        REQUIRE(*result == expected);
    }

    // Queue should now appear empty
    REQUIRE(deque.empty());
}

TEST_CASE("Pop fails on an empty deque", "[concurrent_deque]") {
    concurrent_deque<int, reclamation_technique::bounded> deque(8);
    auto result = deque.pop();
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == PopFailureReason::EmptyQueue);
}

TEST_CASE("Steal fails on an empty deque", "[concurrent_deque]") {
    concurrent_deque<int, reclamation_technique::bounded> deque(8);
    auto result = deque.steal();
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == StealFailureReason::EmptyQueue);
}

TEST_CASE("Flavor correctness: LIFO vs FIFO", "[concurrent_deque][flavor]") {
    concurrent_deque<int, reclamation_technique::bounded, Flavor::Lifo> lifo(8);
    concurrent_deque<int, reclamation_technique::bounded, Flavor::Fifo> fifo(8);

    for (int i = 1; i <= 3; ++i) {
        REQUIRE(lifo.push(i));
        REQUIRE(fifo.push(i));
    }

    auto lifo_result = lifo.pop();
    auto fifo_result = fifo.pop();

    REQUIRE(*lifo_result == 3); // Last in, first out
    REQUIRE(*fifo_result == 1); // First in, first out
}

TEST_CASE("Test with many concurrent stealers", "[concurrent_deque][concurrency]") {
    concurrent_deque<int, reclamation_technique::bounded> deque(200);

    for (int i = 0; i < 200; ++i) {
        REQUIRE(deque.push(i));
    }

    std::atomic total_stolen = 0;
    constexpr int num_threads = 24;

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&] {
            while (true) {
                auto result = deque.steal();
                if (result) {
                    ++total_stolen;
                } else if (result.error() == StealFailureReason::EmptyQueue) {
                    break;
                }
            }
        });
    }

    for (auto& t : threads) t.join();
    REQUIRE(total_stolen == 200);
}

TEST_CASE("Deferred deque resizes multiple times", "[concurrent_deque][deferred]") {
    concurrent_deque<int> deque(2);

    for (int i = 0; i < 100; ++i) {
        REQUIRE(deque.push(i));
    }

    REQUIRE(deque.size() == 100);
    REQUIRE(deque.capacity() == 128);

    for (int i = 99; i >= 0; --i) {
        auto result = deque.pop();
        REQUIRE(result);
        REQUIRE(*result == i);
    }
}