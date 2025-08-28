#include <future>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <thread>

import mpmc_queue;

using namespace thunder;

struct MoveOnly {
    MoveOnly() noexcept = default;

    explicit MoveOnly(const int val) noexcept
        :
        value(val)
    {}

    ~MoveOnly() noexcept = default;

    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;

    MoveOnly(MoveOnly&& rhs) noexcept
        :
        value(rhs.value)
    {
        rhs.value = 0;
        ++moves;
    }

    MoveOnly& operator=(MoveOnly&& other) noexcept {
        value = other.value;
        other.value = 0;
        ++moves;
        return *this;
    }

    static inline std::atomic<int> moves{0};
    int value{0};
};

using namespace std::chrono_literals;

template <class Pred>
bool wait_until(Pred&& pred, const std::chrono::milliseconds max = 200ms) {
    const auto start = std::chrono::steady_clock::now();

    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > max) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

TEST_CASE("mpmc queue basic push/pop", "[mpmc_queue]") {
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

TEST_CASE("mpmc queue basic push/try_pop", "[mpmc_queue]") {
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

TEST_CASE("mpmc queue basic try_push", "[mpmc_queue]") {
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
        REQUIRE(queue.empty());
    }

    SECTION("try_emplace") {
        REQUIRE(queue.try_emplace(5));
        auto item2 = queue.try_pop();
        REQUIRE(item2.has_value());
        REQUIRE(item2.value() == 5);
    }

    SECTION("try_pop empty returns nullopt") {
        auto item = queue.try_pop();
        REQUIRE_FALSE(item.has_value());
    }

    SECTION("overloads with output reference") {
        REQUIRE(queue.try_push(11));
        int item = 0;
        REQUIRE(queue.try_pop(item));
        REQUIRE(item == 11);

        queue.emplace(12);
        item = 0;
        queue.pop(item);
        REQUIRE(item == 12);
    }

    SECTION("try_push fails when full") {
        // Fill to capacity
        for (int i = 0; i < 4; i++) {
            REQUIRE(queue.try_push(i));
        }

        // One more should fail because the queue is full
        REQUIRE_FALSE(queue.try_push(99));

        std::vector<int> items;
        for (int i = 0; i < 4; i++) {
            items.push_back(queue.pop());
        }
        REQUIRE(items == std::vector<int>{0,1,2,3});
    }
}

TEST_CASE("mpmc queue construction & capacity", "[mpmc_queue]") {
    SECTION("throws on zero capacity") {
        REQUIRE_THROWS_AS(mpmc::queue<int>(0), std::invalid_argument);
    }
    SECTION("reports capacity") {
        const mpmc::queue<int> queue(4);
        REQUIRE(queue.capacity() == 4);
        REQUIRE(queue.size() == 0);
        REQUIRE(queue.empty());
    }
}

TEST_CASE("mpmc queue wrap-around", "[mpmc_queue]") {
    mpmc::queue<int> queue(2);
    constexpr int rounds = 1000;
    for (int i = 0; i < rounds; ++i) {
        REQUIRE(queue.try_push(i));
        auto v = queue.pop();
        REQUIRE(v == i);
    }
    REQUIRE(queue.empty());
}

TEST_CASE("mpmc queue move-only type works (preferMove path)", "[mpmc_queue]") {
    MoveOnly::moves = 0;

    {
        mpmc::queue<MoveOnly> queue(4);
        queue.emplace(1);
        const auto item = queue.pop();
        REQUIRE(item.value == 1);

        MoveOnly mv(5);
        queue.push(std::move(mv));
        const auto item2 = queue.pop();
        REQUIRE(item2.value == 5);
    }

    REQUIRE(MoveOnly::moves.load() >= 5);
}

TEST_CASE("mpmc queue pop blocks until item available", "[mpmc_queue]") {
    mpmc::queue<int> queue(1);
    std::promise<void> started;
    std::atomic popped{false};

    auto consumer = std::async(std::launch::async, [&]{
        started.set_value();
        const auto item = queue.pop();
        REQUIRE(item == 99);
        popped = true;
    });

    started.get_future().wait();

    std::this_thread::sleep_for(30ms);
    REQUIRE_FALSE(popped.load());

    queue.push(99);
    REQUIRE(wait_until([&]{ return popped.load(); }, 200ms));
}

TEST_CASE("mpmc queue push blocks when full until consumer makes space", "[mpmc_queue]") {
    mpmc::queue<int> queue(1);
    queue.push(1); // queue now full

    std::promise<void> producer_ready;
    std::atomic pushed{false};

    auto producer = std::async(std::launch::async, [&]{
        producer_ready.set_value();
        queue.push(2); // should block until we pop()
        pushed = true;
    });

    producer_ready.get_future().wait();

    std::this_thread::sleep_for(30ms);
    REQUIRE_FALSE(pushed.load());

    // Free a slot
    REQUIRE(queue.pop() == 1);

    REQUIRE(wait_until([&]{ return pushed.load(); }, 200ms));
    REQUIRE(queue.pop() == 2);
    REQUIRE(queue.empty());
}

TEST_CASE("mpmc queue multi-producer / multi-consumer correctness", "[mpmc_queue][stress]") {
    const auto producers = std::thread::hardware_concurrency() / 2;
    const auto consumers = std::thread::hardware_concurrency() / 2;
    constexpr auto items_per_producer = 25000;
    const auto total = producers * items_per_producer;

    mpmc::queue<int> queue(2048);

    std::atomic produced{0};
    std::atomic consumed{0};
    std::atomic<long long> sumProduced{0};
    std::atomic<long long> sumConsumed{0};

    std::atomic ready{0};
    std::atomic go{false};

    auto produce = [&](const int id){
        ready.fetch_add(1);
        while (!go.load()) { std::this_thread::yield(); }

        for (int i = 0; i < items_per_producer; i++) {
            const auto val = id * items_per_producer + i;
            queue.emplace(val);

            produced.fetch_add(1, std::memory_order_relaxed);
            sumProduced.fetch_add(val, std::memory_order_relaxed);
        }
    };

    auto consume = [&]{
        ready.fetch_add(1);
        while (!go.load()) {
            std::this_thread::yield();
        }

        for (;;) {
            const auto before = consumed.load(std::memory_order_relaxed);
            if (before >= total) {
                break;
            }

            if (auto opt = queue.try_pop()) {
                sumConsumed.fetch_add(*opt, std::memory_order_relaxed);
                consumed.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(producers + consumers);

    for (int i = 0; i < producers; i++) {
        threads.emplace_back(produce, i);
    }
    for (int i = 0; i < consumers; i++) {
        threads.emplace_back(consume);
    }

    while (ready.load() < producers + consumers) {
        std::this_thread::yield();
    }
    go = true;

    for (auto& t : threads) {
        t.join();
    }

    REQUIRE(produced.load() == total);
    REQUIRE(consumed.load() == total);
    REQUIRE(sumConsumed.load() == sumProduced.load());
    REQUIRE(queue.empty());
    REQUIRE(queue.size() == 0);
}
