#include <benchmark/benchmark.h>
#include <atomic>
#include <thread>
#include <cstddef>
#include <latch>

#include "../boost_spsc_queue.h"
#include "../readerwriterqueue.h"
#include "../drogalis_queue.h"
#include "../fine_grained_mutex_queue.h"
#include "../fine_grained_spinlock_queue.h"
#include "../locked_queue.h"
#include "../ProducerConsumerQueue.h"
#include "../rigtorp_spsc.h"
#include "../spinlock_queue.h"

import spsc_queue;

using namespace thunder;

#define NOMINMAX
#include <windows.h>

static void set_high_process_priority() {
    if (!SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS)) {
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    }
}

static void pin_to_cpu(std::thread& thread, const DWORD cpuIndex) {
    const auto handle = thread.native_handle();
    const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << cpuIndex;
    SetThreadAffinityMask(handle, mask);
    //SetThreadPriority(handle, THREAD_PRIORITY_HIGHEST);
    SetThreadPriority(handle, THREAD_PRIORITY_TIME_CRITICAL);
    SetThreadPriorityBoost(handle, TRUE);
}

static void pin_current_thread(DWORD cpuIndex) {
    const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << cpuIndex;
    SetThreadAffinityMask(GetCurrentThread(), mask);
    //SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    SetThreadPriorityBoost(GetCurrentThread(), TRUE);
}

static uint64_t rdtsc_start() {
    _mm_lfence();
    return __rdtsc();
}

static uint64_t rdtsc_end() {
    unsigned aux;
    const uint64_t ts = __rdtscp(&aux);
    _mm_lfence();
    return ts;
}

double calibrate_cycles_per_ns() {
    using namespace std::chrono_literals;
    std::this_thread::sleep_for(100ms);

    const uint64_t t0 = rdtsc_start();
    const auto c0 =  std::chrono::steady_clock::now();

    std::this_thread::sleep_for(500ms);

    const uint64_t t1 = rdtsc_end();
    const auto c1 =  std::chrono::steady_clock::now();

    const auto ns_elapsed =  std::chrono::duration_cast< std::chrono::nanoseconds>(c1 - c0).count();
    const uint64_t cycles_elapsed = t1 - t0;

    return static_cast<double>(cycles_elapsed) / static_cast<double>(ns_elapsed);
}

static double cycles_per_ns() {
    static const double v = calibrate_cycles_per_ns();
    return v;
}

struct Stats {
    double mean{}, min{}, max{}, p50{}, p90{}, p99{};
};

inline void spin_pause(unsigned n = 64) noexcept {
    for (unsigned i = 0; i < n; ++i) _mm_pause();
}

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

    set_high_process_priority();
    pin_current_thread(4);

    std::thread producer([&] {
        while (!start.load(std::memory_order_relaxed)) { /* spin */ }

        T item{};
        while (!stop.load(std::memory_order_relaxed)) {
            if (!queue.try_push(item)) {
                spin_pause();
            }
        }
    });

    uint64_t localCounter = 0;
    std::thread consumer([&] {
        while (!start.load(std::memory_order_relaxed)) { /* spin */ }

        T item;
        while (!stop.load(std::memory_order_relaxed)) {
            if (queue.try_pop(item)) {
                ++localCounter;
            }
            else {
                spin_pause();
            }
        }
    });

    pin_to_cpu(producer, 0);
    pin_to_cpu(consumer, 2);

    // Let both threads begin
    start.store(true, std::memory_order_relaxed);

    for (auto _ : state) {
        std::this_thread::yield();
    }

    // Stop and join
    stop.store(true, std::memory_order_relaxed);
    producer.join();
    consumer.join();

    ops.fetch_add(localCounter, std::memory_order_relaxed);

    const auto xfers = static_cast<double>(ops.load(std::memory_order_relaxed));
    state.counters["ops_per_sec"] = benchmark::Counter(xfers, benchmark::Counter::kIsRate);
    state.counters["ops_per_ms"]  = benchmark::Counter(xfers / 1000.0, benchmark::Counter::kIsRate);
    state.counters["payload_bytes"] = static_cast<double>(PayloadSize);
}

static void BM_RTT_PingPong_SPSC(benchmark::State& state) {
    constexpr uint32_t consumerCpuIndex = 1;
    const auto capacity = static_cast<std::size_t>(state.range(0));
    const auto iters = state.range(1);

    for (auto _ : state) {
        constexpr uint32_t producerCpuIndex = 0;
        spsc::queue<int> queue1(capacity);
        spsc::queue<int> queue2(capacity);

        std::latch threadSyncer(2);

        std::thread consumer([&]{
            pin_current_thread(consumerCpuIndex);
            threadSyncer.count_down();
            threadSyncer.wait();

            int value;
            for (int64_t i = 0; i < iters; ++i) {
                while (!queue1.try_pop(value)) {
                    _mm_pause();
                }
                while (!queue2.try_push(value)) {
                    _mm_pause();
                }
            }
        });

        pin_current_thread(producerCpuIndex);
        threadSyncer.count_down();
        threadSyncer.wait();

        const uint64_t t0 = rdtsc_start();
        for (int64_t i = 0; i < iters; ++i) {
            while (!queue1.try_push(static_cast<int>(i))) {
                _mm_pause();
            }
            int item;
            while (!queue2.try_pop(item)) {
                _mm_pause();
            }
            benchmark::DoNotOptimize(item);
        }
        const uint64_t t1 = rdtsc_end();

        consumer.join();

        const auto total_cycles = static_cast<double>(t1 - t0);
        const double cycles_ns    = cycles_per_ns();
        const double total_ns     = total_cycles / cycles_ns;
        const double rtt_ns       = total_ns / static_cast<double>(iters);

        state.SetIterationTime(total_ns / 1e9);

        state.counters["rtt_ns"]      = benchmark::Counter(rtt_ns);
        state.counters["one_way_ns"]  = benchmark::Counter(rtt_ns / 2.0);
        state.counters["rtt_cycles"]  = benchmark::Counter(total_cycles / static_cast<double>(iters));
        state.counters["iters"]       = static_cast<double>(iters);
        state.counters["cap"]         = static_cast<double>(capacity);
    }
}

static void BM_Latency_SPSC_Distribution(benchmark::State& state) {
    constexpr uint32_t consumerCpuIndex = 1;

    const auto capacity = static_cast<std::size_t>(state.range(0));
    const int64_t iters = state.range(1);

    constexpr uint32_t samplerate = 128;
    constexpr uint32_t sampleMask = samplerate - 1;

    for (auto _ : state) {
        constexpr uint32_t producerCpuIndex = 0;
        spsc::queue<int> queue1(capacity);
        spsc::queue<int> queue2(capacity);

        std::latch threadSyncer(2);

        std::thread consumer([&]{
            pin_current_thread(consumerCpuIndex);
            threadSyncer.count_down();
            threadSyncer.wait();

            int item;
            for (int64_t i = 0; i < iters; ++i) {
                while (!queue1.try_pop(item)) {
                    _mm_pause();
                }
                while (!queue2.try_push(item)) {
                    _mm_pause();
                }
            }
        });

        pin_current_thread(producerCpuIndex);
        threadSyncer.count_down();
        threadSyncer.wait();

        // Per-iteration samples (cycles)
        std::vector<uint64_t> samples;
        samples.reserve(static_cast<size_t>(iters / samplerate));

        const uint64_t t0_total = rdtsc_start();
        for (int64_t i = 0; i < iters; ++i) {
            constexpr int warmupIterations = 1024;
            const bool takeSample = i >= warmupIterations && (static_cast<uint32_t>(i) & sampleMask) == 0;
            uint64_t t0 = 0;

            if (takeSample) {
                t0 = rdtsc_start();
            }

            while (!queue1.try_push(static_cast<int>(i))) {
                _mm_pause();
            }

            int item;
            while (!queue2.try_pop(item)) _mm_pause();
            benchmark::DoNotOptimize(item);

            if (takeSample) {
                const uint64_t t1 = rdtsc_end();
                samples.push_back(t1 - t0);
            }
        }
        const uint64_t t1_total = rdtsc_end();

        consumer.join();

        const auto totalCycles = static_cast<double>(t1_total - t0_total);
        const double cyclesPerNs     = cycles_per_ns();
        const double totalNs     = totalCycles / cyclesPerNs;
        const double meanRttNs  = totalNs / static_cast<double>(iters);

        state.SetIterationTime(totalNs / 1e9);

        std::vector<double> samplesNs;
        samplesNs.reserve(samples.size());
        for (const uint64_t sample : samples) {
            samplesNs.push_back(static_cast<double>(sample) / cyclesPerNs);
        }

        auto percentile = [&](double p)->double {
            if (samplesNs.empty()) {
                return std::numeric_limits<double>::quiet_NaN();
            }

            const size_t idx = static_cast<size_t>(std::clamp(p, 0.0, 100.0) / 100.0 * (samplesNs.size() - 1));
            std::ranges::nth_element(samplesNs, samplesNs.begin() + idx);
            return samplesNs[idx];
        };

        const double p50_ns   = percentile(50.0);
        const double p90_ns   = percentile(90.0);
        const double p99_ns   = percentile(99.0);
        const double p999_ns  = percentile(99.9);

        state.counters["rtt_ns_mean"] = benchmark::Counter(meanRttNs);
        state.counters["rtt_ns_p50"] = benchmark::Counter(p50_ns);
        state.counters["rtt_ns_p90"] = benchmark::Counter(p90_ns);
        state.counters["rtt_ns_p99"] = benchmark::Counter(p99_ns);
        state.counters["rtt_ns_p99_9"] = benchmark::Counter(p999_ns);
        state.counters["iters"] = static_cast<double>(iters);
        state.counters["cap"] = static_cast<double>(capacity);
    }
}

// BENCHMARK_TEMPLATE(BM_SPSC_Throughput, 4);
// BENCHMARK_TEMPLATE(BM_SPSC_Throughput, 8);
// BENCHMARK_TEMPLATE(BM_SPSC_Throughput, 16);
// BENCHMARK_TEMPLATE(BM_SPSC_Throughput, 32);
// BENCHMARK_TEMPLATE(BM_SPSC_Throughput, 64);
// BENCHMARK_TEMPLATE(BM_SPSC_Throughput, 128);
// BENCHMARK_TEMPLATE(BM_SPSC_Throughput, 256);
// BENCHMARK_TEMPLATE(BM_SPSC_Throughput, 512);
//BENCHMARK(BM_RTT_PingPong_SPSC)->UseManualTime()->Args({1000000, 10000000});
BENCHMARK(BM_Latency_SPSC_Distribution)->UseManualTime()->Args({1000000, 10000000});

BENCHMARK_MAIN();