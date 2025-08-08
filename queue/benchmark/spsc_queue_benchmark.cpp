#include <benchmark/benchmark.h>
#include <atomic>
#include <thread>
#include <cstddef>
#include "../spsc_queue.h"

using namespace thunder;

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
static void pin_to_cpu(std::thread& thread, const DWORD cpuIndex) {
    const auto handle = thread.native_handle();
    const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << cpuIndex;
    SetThreadAffinityMask(handle, mask);
    SetThreadPriority(handle, THREAD_PRIORITY_HIGHEST);
}
#else
static void pin_to_cpu(std::thread&, int) {}
#endif

template<std::size_t N>
struct Payload {
    alignas(64) std::array<std::byte, N> bytes{};
};

template<std::size_t PayloadSize>
static void BM_SPSC_Throughput(benchmark::State& state) {
    using T = Payload<PayloadSize>;

    // Make capacity reasonably large to reduce artificial backpressure, but not huge
    spsc::queue<T> queue(1024);

    std::atomic start{false};
    std::atomic stop{false};
    std::atomic<uint64_t> ops{0}; // count successful push+pop pairs

    std::thread producer([&] {
        while (!start.load(std::memory_order_relaxed)) { /* spin */ }

        T item{};

        while (!stop.load(std::memory_order_relaxed)) {
            if (queue.try_push(std::move(item))) {
                ++ops;
            }
            else {
               std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&] {
        while (!start.load(std::memory_order_relaxed)) { /* spin */ }

        while (!stop.load(std::memory_order_relaxed)) {
            auto item = queue.try_pop();
            if (!item) {
                std::this_thread::yield();
            }
        }
    });

    pin_to_cpu(producer, 0);
    pin_to_cpu(consumer, 2);

    // Let both threads begin
    start.store(true, std::memory_order_relaxed);

    for (auto _ : state) {
        // Do nothing: the worker threads do the work concurrently during the benchmark window.
        // We just sleep/yield a tad to avoid busy driving here.
        std::this_thread::yield();
    }

    // Stop and join
    stop.store(true, std::memory_order_relaxed);
    producer.join();
    consumer.join();

    state.counters["ops_per_sec"] = benchmark::Counter(
        static_cast<double>(ops.load(std::memory_order_relaxed)),
        benchmark::Counter::kIsRate
    );
    state.counters["ops_per_ms"] = benchmark::Counter(
        static_cast<double>(ops.load(std::memory_order_relaxed)) / 1000.0,
        benchmark::Counter::kIsRate
    );

    //expose payload size in bytes
    state.counters["payload_bytes"] = static_cast<double>(PayloadSize);
}

BENCHMARK_TEMPLATE(BM_SPSC_Throughput, 4);
BENCHMARK_TEMPLATE(BM_SPSC_Throughput, 8);
BENCHMARK_TEMPLATE(BM_SPSC_Throughput, 16);
BENCHMARK_TEMPLATE(BM_SPSC_Throughput, 32);
BENCHMARK_TEMPLATE(BM_SPSC_Throughput, 64);
BENCHMARK_TEMPLATE(BM_SPSC_Throughput, 128);
BENCHMARK_TEMPLATE(BM_SPSC_Throughput, 256);
BENCHMARK_TEMPLATE(BM_SPSC_Throughput, 512);
BENCHMARK_TEMPLATE(BM_SPSC_Throughput, 1024);
BENCHMARK_TEMPLATE(BM_SPSC_Throughput, 2048);
BENCHMARK_TEMPLATE(BM_SPSC_Throughput, 4096);

BENCHMARK_MAIN();