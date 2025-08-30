module;
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <latch>
#include <optional>
#include <thread>
#include <vector>
#include <coroutine>
#include <libassert/assert.hpp>
export module cpu_scheduler;

import concurrent_deque;
import mpmc_queue;
import thread_parker;
import backoff;
import coro;

namespace thunder::cpu {
    struct sleeper_node {
        sleeper_node() = default;
        sleeper_node(const sleeper_node&) = delete;
        sleeper_node& operator=(const sleeper_node&) = delete;

        // We need these move constructors to store sleeper_node in a vector.
        // A std::vector<T> requires 'T' to be move-constructible when it grows/reallocates.
        // std::vector<sleeper_node>::resize(threadCount) is used when constructing the scheduler
        // which only works if sleeper_node is movable. This is fine as long as we’re not concurrently
        // accessing nodes while the vector is reallocating, which can never happen with the current implementation.
        sleeper_node(sleeper_node&& other) noexcept {
            next.store(other.next.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        sleeper_node& operator=(sleeper_node&& other) noexcept {
            next.store(other.next.load(std::memory_order_relaxed), std::memory_order_relaxed);
            return *this;
        }
        ~sleeper_node() = default;

        std::atomic_int next{-1};
    };

    class sleeper_stack {
    public:
        explicit sleeper_stack(const size_t capacity) {
            nodes.resize(capacity);

            // std::memory_order_relaxed is fine here. The object is not shared/published yet.
            m_head.store(pack(-1, 0), std::memory_order_relaxed);
        }

        void push(const int index) noexcept {
            for (;;) {
                // std::memory_order_relaxed is fine here. We just want the value, we don't
                // consume any data that depends on prior writes from whoever set m_head.
                auto head = m_head.load(std::memory_order_relaxed);

                const auto oldIndex = unpack_index(head);
                const auto tag = unpack_tag(head);

                // std::memory_order_relaxed is fine here. The write is not published yet so no ordering is needed here.
                nodes[index].next.store(oldIndex, std::memory_order_relaxed);

                // If the CAS succeeds, we need std::memory_order_release to establish a happens-before relationship
                // with any subsequent pop's (i.e., we publish/release the nodes[index].next.store() write).
                // On failure std::memory_order_relaxed is fine because we will just retry the loop.
                if (m_head.compare_exchange_weak(head, pack(index, tag + 1), std::memory_order_release, std::memory_order_relaxed)) {
                    return;
                }
            }
        }

        std::optional<int> pop() noexcept {
            for (;;) {
                // std::memory_order_acquire is needed here because it pairs with the push() that stored this head.
                // If we see a head that was released by push(), we also see that nodes ´next´ value.
                auto head = m_head.load(std::memory_order_acquire);

                const auto index = unpack_index(head);

                if (index == -1) {
                    return std::nullopt;
                }

                const auto tag = unpack_tag(head);

                // std::memory_order_relaxed here is fine because of the acquire load of head above.
                const auto next = nodes[index].next.load(std::memory_order_relaxed);

                // std::memory_order_relaxed is fine for the success order. We do not publish any new writes here.
                // std::memory_order_relaxed is also fine for the failure order, because we loop again and retry with a new
                // aquire load of m_head.
                if (m_head.compare_exchange_weak(head, pack(next, tag + 1), std::memory_order_relaxed, std::memory_order_relaxed)) {
                    return index;
                }
            }
        }
    private:
        static constexpr uint64_t pack(const int index, const uint32_t tag) noexcept {
            return (static_cast<uint64_t>(tag) << 32) | static_cast<uint32_t>(index);
        }
        static constexpr int unpack_index(const uint64_t node) noexcept { return static_cast<int>(static_cast<uint32_t>(node)); }
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
                if (m_task) {
                    (*m_task)();
                }
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

            for (int i = 0; i < threadCount; i++) {
                m_localQueues.push_back(std::make_unique<concurrent_deque<task_type>>());
                m_threads.emplace_back([this, i](std::stop_token stopToken) {
                    this->worker(stopToken, i);
                });
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

            push_task(std::move(task));

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

            wake_one_thread();
        }

        std::optional<task_type> steal_task() const noexcept {
            for (int i = 0; i < m_localQueues.size(); i++) {
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
                owned->is_coro()
                    ? owned->get_coro_handle().resume()
                    : owned->invoke();
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
            m_sleepers.push(queueIndex);

            m_parkingLot[queueIndex]->park();
            m_sleeping.fetch_sub(1, std::memory_order_seq_cst);
            backoff.reset();
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
        std::vector<std::jthread> m_threads;

        std::vector<std::unique_ptr<thread_parker>> m_parkingLot;
        std::latch m_threadReadyBarrier;

        std::atomic<uint32_t> m_sleeping{0};
        sleeper_stack m_sleepers;
    };
}