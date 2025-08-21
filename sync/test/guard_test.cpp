#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <vector>
#include <atomic>
#include <latch>
#include <chrono>
#include <shared_mutex>
#include <type_traits>

import guard;

using namespace thunder;

struct Counter { int number = 0; };

struct Probe {
    int x = 0;
    int inc() { return ++x; }
    [[nodiscard]] int get() const { return x; }
};

template<class T>
concept HasRead = requires(T& t) { t.read(); };

template<class T>
concept HasWrite = requires(T& t) { t.write(); };

struct CountingSharedMutex {
    std::shared_mutex inner;
    std::atomic<int> lock_calls{0};
    std::atomic<int> try_lock_calls{0};
    std::atomic<int> unlock_calls{0};
    std::atomic<int> lock_shared_calls{0};
    std::atomic<int> unlock_shared_calls{0};
    std::atomic<int> try_lock_shared_calls{0};

    auto try_lock() { ++try_lock_calls;  return inner.try_lock(); }
    void lock() { ++lock_calls; inner.lock(); }
    void unlock() { ++unlock_calls; inner.unlock(); }
    void lock_shared() { ++lock_shared_calls; inner.lock_shared(); }
    void unlock_shared() { ++unlock_shared_calls; inner.unlock_shared(); }
    auto try_lock_shared() { ++try_lock_shared_calls; return inner.try_lock_shared(); }
};

TEST_CASE("read()/write() exist only for SharedMutexLike mutexes", "[guard]") {
    guard<int, std::mutex> guard1;
    guard<int, std::shared_mutex> guard2;

    STATIC_REQUIRE(!HasRead<decltype(guard1)>);
    STATIC_REQUIRE(!HasWrite<decltype(guard1)>);

    STATIC_REQUIRE(HasRead<decltype(guard2)>);
    STATIC_REQUIRE(HasWrite<decltype(guard2)>);
}

TEST_CASE("native_handle types are correct", "[guard]") {
    guard<int, std::shared_mutex> guard;
    STATIC_REQUIRE(std::same_as<decltype(guard.native_handle()), std::shared_mutex&>);
    STATIC_REQUIRE(std::same_as<decltype(std::as_const(guard).native_handle()), const std::shared_mutex&>);
}

TEST_CASE("locked_ptr basic operators and move-only semantics", "[guard]") {
    guard<Probe> guard;
    {
        auto lock = guard.lock();
        STATIC_REQUIRE(!std::is_copy_constructible_v<decltype(lock)>);
        STATIC_REQUIRE(std::is_move_constructible_v<decltype(lock)>);

        REQUIRE(lock->inc() == 1);
        REQUIRE((*lock).get() == 1);
        REQUIRE(std::addressof(lock.get()) == std::addressof(*lock));

        // move the locked_ptr; lock is transferred, still exactly one lock held
        auto p2 = std::move(lock);
        REQUIRE(p2->inc() == 2);
    }

    auto val = guard.with_read_lock([](Probe const& pr){ return pr.get(); });
    REQUIRE(val == 2);
}

TEST_CASE("locked_ptr exposes its lock object for advanced scenarios", "[guard]") {
    guard<int> guard;
    auto lock = guard.lock();
    auto& lk = lock.lock();
    STATIC_REQUIRE(std::same_as<std::remove_reference_t<decltype(lk)>, std::unique_lock<std::shared_mutex>>);
    REQUIRE(lk.owns_lock());
}

TEST_CASE("with_lock releases mutex if callback throws", "[guard]") {
    guard<int, CountingSharedMutex> guard;
    const auto& mtx = guard.native_handle();

    REQUIRE_THROWS_AS(
        guard.with_lock([&](int& v) -> void {
            v = 123;
            throw std::runtime_error("");
        }),
        std::runtime_error
    );

    guard.with_lock([](int& v){ v += 1; });
    const auto value = guard.with_read_lock([](int const& v){ return v; });
    REQUIRE(value == 124);
    REQUIRE(mtx.lock_calls.load()   == 2);
    REQUIRE(mtx.unlock_calls.load() == 2);
    REQUIRE(mtx.lock_shared_calls.load() == 1);
    REQUIRE(mtx.unlock_shared_calls.load() == 1);
}

TEST_CASE("move-construct transfers value", "[guard]") {
    guard<Probe> guard1;
    guard1.with_lock([](Probe& pr){ pr.x = 7; });

    guard guard2(std::move(guard1));
    auto value = guard2.with_read_lock([](Probe const& pr){ return pr.get(); });
    REQUIRE(value == 7);
}

TEST_CASE("move-construct transfers value", "[guard]") {
    guard<std::vector<int>> guard1;
    guard1.lock()->push_back(1);
}

TEST_CASE("move-assign locks both mutexes and moves value", "[guard]") {
    guard<int, CountingSharedMutex> guard1;
    guard<int, CountingSharedMutex> guard2;

    guard1.with_lock([](int& v){ v = 111; });
    guard2.with_lock([](int& v){ v = 222; });

    auto& mtx1 = guard1.native_handle();
    auto& mtx2 = guard2.native_handle();

    guard2 = std::move(guard1);

    auto value = guard2.with_read_lock([](int const& v){ return v; });
    REQUIRE(value == 111);

    REQUIRE(mtx1.lock_calls.load()   >= 1);
    REQUIRE(mtx1.unlock_calls.load() >= 1);
    REQUIRE(mtx2.lock_calls.load()   >= 1);
    REQUIRE(mtx2.unlock_calls.load() >= 1);
}

TEST_CASE("shared reads can coexist; writer waits", "[guard]") {
    guard<int> guard;
    guard.with_lock([](int& v){ v = 5; });

    std::latch start(1), reads_acquired(2), release_reads(1), writer_done(1);
    std::atomic writer_started{false};
    std::atomic writer_finished{false};

    std::jthread r1([&]{
        start.wait();
        const auto reader = guard.read();
        reads_acquired.count_down();
        release_reads.wait();
        (void)*reader;
    });

    std::jthread r2([&]{
        start.wait();
        const auto reader = guard.read();
        reads_acquired.count_down();
        release_reads.wait();
        (void)*reader;
    });

    std::jthread w([&]{
        start.wait();
        reads_acquired.wait();
        writer_started = true;

        // this should block until readers release
        const auto writer = guard.lock();
        *writer = 42;
        writer_finished = true;
        writer_done.count_down();
    });

    start.count_down();
    reads_acquired.wait();

    // Give the writer a chance to attempt locking while readers still hold
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(writer_started.load());
    REQUIRE_FALSE(writer_finished.load());

    // Release readers; the writer should now complete
    release_reads.count_down();
    writer_done.wait();

    auto final = guard.with_read_lock([](int const& v){ return v; });
    REQUIRE(final == 42);
}

TEST_CASE("operator-> works with read() and const member functions", "[guard]") {
    guard<Probe> guard;
    guard.with_lock([](Probe& pr){ pr.x = 13; });
    const auto reader = guard.read();
    REQUIRE(reader->get() == 13);
}

TEST_CASE("Exclusive lock increments correctly", "[guard]") {
    guard<Counter> guard;
    guard.with_lock([](Counter& counter){ counter.number = 42; });

    const auto val = guard.with_lock([](const Counter& counter){ return counter.number; });
    REQUIRE(val == 42);
}

TEST_CASE("instrumented mutex counts exclusive lock/unlock via guard::lock()", "[guard]") {
    guard<int, CountingSharedMutex> guard;
    {
        auto lock = guard.lock();
        *lock = 123;
    }

    auto& mtx = guard.native_handle();
    REQUIRE(mtx.lock_calls.load() == 1);
    REQUIRE(mtx.unlock_calls.load() == 1);
    REQUIRE(mtx.lock_shared_calls.load() == 0);
    REQUIRE(mtx.unlock_shared_calls.load() == 0);
}

TEST_CASE("concurrent increments are correct", "[guard]") {
    guard<Counter> guard;
    constexpr int threads = 4;
    constexpr int iters = 1000;

    {
        std::vector<std::jthread> ts;
        for (int i=0;i<threads;i++) {
            ts.emplace_back([&]{
              for (int j=0;j<iters;j++) guard.with_lock([](Counter& c){ c.number++; });
            });
        }
    }

    auto total = guard.with_read_lock([](Counter const& c){ return c.number; });
    REQUIRE(total == threads * iters);
}

TEST_CASE("instrumented mutex counts shared lock/unlock via guard::read()", "[guard]") {
    guard<int, CountingSharedMutex> guard;
    guard.with_lock([](int& v){ v = 7; });

    {
        auto reader1 = guard.read();
        auto reader2 = guard.read();
        (void)*reader1; (void)*reader2;
    }

    auto& m = guard.native_handle();
    REQUIRE(m.lock_shared_calls.load() == 2);
    REQUIRE(m.unlock_shared_calls.load() == 2);
    REQUIRE(m.lock_calls.load() == 1);
    REQUIRE(m.unlock_calls.load() == 1);
}