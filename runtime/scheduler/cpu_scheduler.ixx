module;
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <latch>
#include <optional>
#include <thread>
#include <vector>
export module cpu_scheduler;

import concurrent_deque;
import mpmc_queue;
import thread_parker;
import backoff;

namespace thunder::cpu {
    struct sleeper_node {
        sleeper_node() = default;
        sleeper_node(const sleeper_node&) = delete;
        sleeper_node& operator=(const sleeper_node&) = delete;

        sleeper_node(sleeper_node&& other) noexcept {
            next.store(other.next.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        sleeper_node& operator=(sleeper_node&& other) noexcept {
            next.store(other.next.load(std::memory_order_relaxed), std::memory_order_relaxed);
            return *this;
        }

        std::atomic_int next{-1};
    };

    class sleeper_stack {
    public:
        explicit sleeper_stack(const size_t capacity) {
            nodes.resize(capacity);
            m_head.store(pack(-1, 0), std::memory_order_relaxed);
        }

        void push(const int index) {
            for (;;) {
                auto head = m_head.load(std::memory_order_acquire);

                const auto oldIndex = unpack_index(head);
                const auto tag = unpack_tag(head);

                nodes[index].next.store(oldIndex, std::memory_order_relaxed);

                if (m_head.compare_exchange_weak(head, pack(index, tag + 1), std::memory_order_acq_rel, std::memory_order_acquire)) {
                    return;
                }
            }
        }

        std::optional<int> pop() noexcept {
            for (;;) {
                auto head = m_head.load(std::memory_order_acquire);

                const auto index = unpack_index(head);

                if (index == -1) {
                    return std::nullopt;
                }

                const auto tag = unpack_tag(head);
                const auto next = nodes[index].next.load(std::memory_order_relaxed);

                if (m_head.compare_exchange_weak(head, pack(next, tag + 1), std::memory_order_acq_rel, std::memory_order_acquire)) {
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
    };

    export class scheduler {
    public:
        using task_type = task_wrapper*;

        explicit scheduler(const uint32_t threadCount = std::thread::hardware_concurrency())
            :
            m_globalQueue(1024),
            m_threadReadyBarrier(threadCount),
            m_sleepers(threadCount)
        {
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
        ~scheduler() {
            for (auto& thread : m_threads) {
                thread.request_stop();
            }
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

        std::optional<task_type> steal_task() const {
            for (int i = 0; i < m_localQueues.size(); i++) {
                const auto index = (m_localQueueIndex + i + 1) % m_localQueues.size();

                if (auto task = m_localQueues[index]->steal()) {
                    return task.value();
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] bool try_invoke_task() {
            if (const auto task = m_localQueue->pop()) {
                const std::unique_ptr<task_wrapper> owned(task.value());
                owned->invoke();
                return true;
            }
            if (const auto task = m_globalQueue.try_pop()) {
                const std::unique_ptr<task_wrapper> owned(task.value());
                owned->invoke();
                return true;
            }
            if (const auto task = steal_task()) {
                std::unique_ptr<task_wrapper> const owned(task.value());
                owned->invoke();
                return true;
            }
            return false;
        }

        void park_thread(const uint32_t queueIndex, backoff& backoff) {
            m_sleeping.fetch_add(1, std::memory_order_seq_cst);
            m_sleepers.push(queueIndex);

            m_parkingLot[queueIndex]->park();
            m_sleeping.fetch_sub(1, std::memory_order_seq_cst);
            backoff.reset();
        }

        void wake_one_thread() {
            if (m_sleeping.load(std::memory_order_seq_cst) > 0) {
                if (const auto idx = m_sleepers.pop()) {
                    m_parkingLot[idx.value()]->unpark();
                }
            }
        }

        void worker(std::stop_token stopToken, uint32_t queueIndex) {
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
