module;
//windows.h is such a stupid header
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define STRICT
#define UNICODE

#include <windows.h>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <latch>
#include <optional>
#include <thread>
#include <vector>
#include <coroutine>
#include <span>
#include <chrono>
#include <libassert/assert.hpp>
export module cpu_scheduler;

import concurrent_deque;
import mpmc_queue;
import thread_parker;
import backoff;
import coro;

using namespace std::chrono_literals;

namespace thunder::cpu {

    class thunder_thread {
    public:
        using id = std::jthread::id;
        using native_handle_type = std::jthread::native_handle_type;

        thunder_thread() noexcept = default;

        template <class Fn, class... Args>
        requires (!std::is_same_v<std::remove_cvref_t<Fn>, thunder_thread>)
        [[nodiscard]] explicit thunder_thread(Fn&& fn, Args&&... args)
            :
            m_thread(std::forward<Fn>(fn), std::forward<Args>(args)...)
        {}

        ~thunder_thread() { try_cancel_and_join(); }

        thunder_thread(const thunder_thread&) = delete;
        thunder_thread(thunder_thread&&) noexcept = default;
        thunder_thread& operator=(const thunder_thread&) = delete;

        thunder_thread& operator=(thunder_thread&& rhs) noexcept {
            if (this == std::addressof(rhs)) {
                return *this;
            }

            try_cancel_and_join();
            m_thread = std::move(rhs.m_thread);
            return *this;
        }

        void swap(thunder_thread& rhs) noexcept { m_thread.swap(rhs.m_thread); }
        [[nodiscard]] bool joinable() const noexcept { return m_thread.joinable(); }
        void join() { m_thread.join(); }
        void detach() { m_thread.detach(); }
        [[nodiscard]] id get_id() const noexcept { return m_thread.get_id(); }
        [[nodiscard]] native_handle_type native_handle() noexcept { return m_thread.native_handle(); }
        [[nodiscard]] std::stop_source get_stop_source() noexcept { return m_thread.get_stop_source(); }
        [[nodiscard]] std::stop_token get_stop_token() const noexcept { return m_thread.get_stop_token(); }
        bool request_stop() noexcept { return m_thread.request_stop(); }

        void set_thread_name(const std::string_view name) noexcept {
#ifdef _WIN32
            const auto length = MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
            if (length > 0) {
                std::wstring wideName(length, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), wideName.data(), length);
                [[maybe_unused]] auto result = SetThreadDescription(m_thread.native_handle(), wideName.c_str());
            }
#endif
        }

        friend void swap(thunder_thread& lhs, thunder_thread& rhs) noexcept {
            lhs.m_thread.swap(rhs.m_thread);
        }

        [[nodiscard]] static unsigned int hardware_concurrency() noexcept { return std::thread::hardware_concurrency(); }
    private:
        void try_cancel_and_join() noexcept {
            if (m_thread.joinable()) {
                m_thread.request_stop();
                m_thread.join();
            }
        }
        std::jthread m_thread;
    };

    constexpr auto target_global_queue_interval = static_cast<double>(200ns .count());
    constexpr auto target_tasks_polled_per_global_queue_interval = 61.0;
    constexpr auto task_poll_time_ewma_alpha = 0.1;

    struct worker_stats {
        double task_poll_time_ewma = target_global_queue_interval / target_tasks_polled_per_global_queue_interval;
        uint64_t batch_start = 0;
        uint32_t tasks_polled_in_batch = 0;
    };

    static thread_local worker_stats wstats;

    inline uint64_t now_ns() noexcept {
        using namespace std::chrono;
        return std::chrono::duration_cast<nanoseconds>(high_resolution_clock::now().time_since_epoch()).count();
    }

    void start_processing_scheduled_tasks() {
        wstats.batch_start = now_ns();
        wstats.tasks_polled_in_batch = 0;
    }

    void end_processing_scheduled_tasks() {
        if (wstats.tasks_polled_in_batch > 0) {
            const auto now = now_ns();

            const auto elapsed = now - wstats.batch_start;
            const auto numPolls = static_cast<double>(wstats.tasks_polled_in_batch);

            const auto meanPollDuration = elapsed / numPolls;
            const double weighted_alpha  = 1.0 - std::pow(1.0 - task_poll_time_ewma_alpha, numPolls);
            wstats.task_poll_time_ewma = weighted_alpha * meanPollDuration + (1.0 - weighted_alpha) * wstats.task_poll_time_ewma;
        }
    }

    struct sleeper_node {
        std::int32_t next{-1};
    };

    class sleeper_stack {
    public:
        explicit sleeper_stack(const size_t capacity) {
            nodes.resize(capacity);

            // std::memory_order_relaxed is fine here. The object is not shared/published yet.
            m_head.store(pack(-1, 0), std::memory_order_relaxed);
        }

        void push(const int32_t index) noexcept {
            DEBUG_ASSERT(index >= 0 && index < static_cast<int32_t>(nodes.size()));

            for (;;) {
                // std::memory_order_relaxed is fine here. We just want the value, we don't
                // consume any data that depends on prior writes from whoever set m_head.
                auto head = m_head.load(std::memory_order_relaxed);

                const auto oldIndex = unpack_index(head);
                const auto tag = unpack_tag(head);

                nodes[index].next = oldIndex;

                // If the CAS succeeds, we need std::memory_order_release to establish a happens-before relationship
                // with any subsequent pop's (i.e., we publish/release the nodes[index].next.store() write).
                // On failure std::memory_order_relaxed is fine because we will just retry the loop.
                if (m_head.compare_exchange_weak(head, pack(index, tag + 1), std::memory_order_release, std::memory_order_relaxed)) {
                    return;
                }
            }
        }

        std::optional<int32_t> pop() noexcept {
            for (;;) {
                // std::memory_order_acquire is needed here because it pairs with the push() that stored this head.
                // If we see a head that was released by push(), we also see that nodes ´next´ value.
                auto head = m_head.load(std::memory_order_acquire);

                const auto index = unpack_index(head);

                if (index == -1) {
                    return std::nullopt;
                }

                DEBUG_ASSERT(index < static_cast<int32_t>(nodes.size()));

                const auto tag = unpack_tag(head);

                const auto next = nodes[index].next;

                // std::memory_order_relaxed is fine for the success order. We do not publish any new writes here.
                // std::memory_order_relaxed is also fine for the failure order, because we loop again and retry with a new
                // aquire load of m_head.
                if (m_head.compare_exchange_weak(head, pack(next, tag + 1), std::memory_order_relaxed, std::memory_order_relaxed)) {
                    return index;
                }
            }
        }
    private:
        static constexpr uint64_t pack(const int32_t index, const uint32_t tag) noexcept {
            return (static_cast<uint64_t>(tag) << 32) | static_cast<uint32_t>(index);
        }
        static constexpr int32_t unpack_index(const uint64_t node) noexcept { return static_cast<int>(static_cast<uint32_t>(node)); }
        static constexpr uint32_t unpack_tag(const uint64_t node) noexcept { return static_cast<uint32_t>(node >> 32); }

        std::vector<sleeper_node> nodes;
        std::atomic<uint64_t> m_head{ pack(-1, 0) };
    };

    class task_wrapper {
    public:
        using task_handle = std::move_only_function<void()>;
        explicit task_wrapper(task_handle task) noexcept: m_task(std::move(task)) {}
        explicit task_wrapper(std::coroutine_handle<> handle) noexcept : m_coroHandle(handle) {}

        [[nodiscard]] bool is_coro() const noexcept { return m_coroHandle != nullptr; }
        [[nodiscard]] auto get_coro_handle() const noexcept { return m_coroHandle; }

        void invoke() noexcept {
            try {
                if (m_coroHandle) {
                    m_coroHandle.resume();
                }
                else if (m_task) {
                    (*m_task)();
                }
                std::unreachable();
            }
            catch ([[maybe_unused]] const std::exception& e) {

            }
        }
    private:
        std::optional<task_handle> m_task;
        std::coroutine_handle<> m_coroHandle{};
    };

    template<class T>
    concept task_holder =
    requires(T t) {
        { static_cast<bool>(t) } -> std::convertible_to<bool>;
        { t.value() };
    };

    export class scheduler {
    public:
        struct awaiter {
            scheduler* pool{nullptr};

            static constexpr bool await_ready() noexcept { return false; }
            static constexpr void await_resume() noexcept {}
            void await_suspend(std::coroutine_handle<> handle) const noexcept {
                if (handle && pool) {
                    pool->push_task(handle);
                }
            }
        };

        using task_type = task_wrapper*;

        explicit scheduler(const uint32_t threadCount = std::max(1u, std::thread::hardware_concurrency()))
            :
            m_globalQueue(1024),
            m_threadReadyBarrier(threadCount),
            m_sleepers(threadCount)
        {
            DEBUG_ASSERT(threadCount > 0);

            m_localQueues.reserve(threadCount);
            m_threads.reserve(threadCount);
            m_parkingLot.reserve(threadCount);

            for (uint32_t i = 0; i < threadCount; i++) {
                m_parkingLot.emplace_back(std::make_unique<thread_parker>());
            }

            for (uint32_t i = 0; i < threadCount; i++) {
                m_localQueues.push_back(std::make_unique<concurrent_deque<task_type>>());
                m_threads.emplace_back([this, i](std::stop_token stopToken) {
                    this->worker(stopToken, i);
                });
                std::string threadName = std::format("cpu_scheduler_worker_thread_{}", i);
                m_threads[i].set_thread_name(threadName);
            }
        }

        scheduler(const scheduler&) = delete;
        scheduler& operator=(const scheduler&) = delete;
        ~scheduler() noexcept {
            for (auto& thread : m_threads) {
                thread.request_stop();
            }
            for (size_t i = 0; i < m_threads.size(); i++) {
                m_parkingLot[i]->unpark();
            }

            for (auto& thread : m_threads) {
                thread.join();
            }

            while (auto task = m_globalQueue.try_pop()) {
                delete task.value();
            }

            for (const auto& queue : m_localQueues) {
                while (auto task = queue->pop()) {
                    delete task.value();
                }
            }
        }

        [[nodiscard]] static scheduler& global(const uint32_t threadCount = std::thread::hardware_concurrency()) noexcept {
            static scheduler globalInstance(threadCount);
            return globalInstance;
        }

        template<class Fnc, class... Args>
        decltype(auto) submit(Fnc&& fnc, Args... args) {
            using return_type = std::invoke_result_t<Fnc, Args...>;
            std::packaged_task<return_type()> task(std::bind(std::forward<Fnc>(fnc), std::forward<Args>(args)...));

            auto result = task.get_future();

            // wait for workers to become ready before we start pushing tasks into the pool
            m_threadReadyBarrier.wait();

            push_one(std::move(task));

            return result;
        }

        auto sched() { return awaiter{this}; }
    private:
        template<class T>
        void push_task(T&& task) {
            auto taskPtr = std::make_unique<task_wrapper>(std::forward<T>(task));
            task_type rawPtr = taskPtr.release();

            if (m_localQueue) {
                m_localQueue->push(rawPtr);
            }
            else {
                m_globalQueue.push(rawPtr);
            }
        }

        template<class T>
        void push_one(T&& task) {
            push_task(std::forward<T>(task));
            wake_one_thread();
        }

        template<class T>
        void push_many(std::span<T> tasks) {
            for (auto& task : tasks) {
                push_task(std::move(task));
            }
            wake_n_threads(static_cast<uint32_t>(tasks.size()));
        }

        std::optional<task_type> steal_task() const noexcept {
            for (uint32_t i = 0; i < m_localQueues.size(); i++) {
                const auto index = (m_localQueueIndex + i + 1) % m_localQueues.size();

                DEBUG_ASSERT(m_localQueues[index] != nullptr);

                if (auto task = m_localQueues[index]->steal()) {
                    return task.value();
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] bool try_invoke_task() noexcept {
            DEBUG_ASSERT(m_localQueue != nullptr);

            [[nodiscard]] auto try_invoke_from = []<task_holder T>(T task)  noexcept -> bool {
                if (!task) {
                    return false;
                }

                const std::unique_ptr<task_wrapper> owned(task.value());
                owned->invoke();
                return true;
            };

            if (try_invoke_from(m_localQueue->pop())) {
                return true;
            }

            if (try_invoke_from(m_globalQueue.try_pop())) {
                return true;
            }

            if (try_invoke_from(steal_task())) {
                return true;
            }
            return false;
        }

        void park_thread(const uint32_t queueIndex, backoff& backoff) {
            DEBUG_ASSERT(queueIndex < m_parkingLot.size());
            DEBUG_ASSERT(m_parkingLot[queueIndex] != nullptr);

            m_sleeping.fetch_add(1, std::memory_order_seq_cst);
            m_sleepers.push(static_cast<int32_t>(queueIndex));

            m_parkingLot[queueIndex]->park();
            m_sleeping.fetch_sub(1, std::memory_order_seq_cst);
            backoff.reset();
        }

        void wake_n_threads(uint32_t n) {
            const auto cap = static_cast<uint32_t>(m_threads.size());
            n = std::min(n, cap);

            for (uint32_t i = 0; i < n; i++) {
                if (m_sleeping.load(std::memory_order_seq_cst) == 0) {
                     break;
                }

                if (const auto idx = m_sleepers.pop()) {
                    DEBUG_ASSERT(m_parkingLot[idx.value()] != nullptr);
                    m_parkingLot[idx.value()]->unpark();
                }
                else {
                    break;
                }
            }
        }

        void wake_one_thread() {
            if (m_sleeping.load(std::memory_order_seq_cst) > 0) {
                if (const auto idx = m_sleepers.pop()) {
                    DEBUG_ASSERT(m_parkingLot[idx.value()] != nullptr);

                    m_parkingLot[idx.value()]->unpark();
                }
            }
        }

        void worker(std::stop_token stopToken, uint32_t queueIndex) {
            DEBUG_ASSERT(queueIndex < m_localQueues.size());
            DEBUG_ASSERT(m_localQueues[queueIndex] != nullptr);

            m_localQueueIndex = queueIndex;
            m_localQueue = m_localQueues[queueIndex].get();
            m_threadReadyBarrier.count_down();

            backoff backoff;
            while (!stopToken.stop_requested()) {
                const auto invokeResult = try_invoke_task();

                // there were no tasks to execute
                if (!invokeResult) {

                    // spin a little, then retry
                    backoff.snooze();
                    if (!backoff.is_completed()) {
                        continue;
                    }

                    // still no work available after spinning. park the thread.
                    park_thread(queueIndex, backoff);
                }
            }
        }

        mpmc::queue<task_type> m_globalQueue;
        std::vector<std::unique_ptr<concurrent_deque<task_type>>> m_localQueues;

        static inline thread_local concurrent_deque<task_type>* m_localQueue = nullptr;
        static inline thread_local uint32_t m_localQueueIndex = 0;
        std::vector<thunder_thread> m_threads;

        std::vector<std::unique_ptr<thread_parker>> m_parkingLot;
        std::latch m_threadReadyBarrier;

        std::atomic<uint32_t> m_sleeping{0};
        sleeper_stack m_sleepers;
    };
}