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

namespace thunder::cpu {

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
            catch (const std::exception& e) {

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
            m_threadReadyBarrier(threadCount)
        {
            m_localQueues.reserve(threadCount);
            m_threads.reserve(threadCount);
            for (int i = 0; i < threadCount; i++) {
                m_localQueues.push_back(std::make_unique<concurrent_deque<task_type>>());
                m_threads.emplace_back([this, i](std::stop_token stopToken) {
                    this->worker(stopToken, i);
                });
            }
        }
        scheduler(const scheduler&) = delete;
        scheduler& operator=(const scheduler&) = delete;
        ~scheduler() = default;

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
        }

        std::optional<task_type> steal_task() {
            for (int i = 0; i < m_localQueues.size(); i++) {
                const auto index = (m_localQueueIndex + i + 1) % m_localQueues.size();

                if (auto task = m_localQueues[index]->steal()) {
                    return task.value();
                }
            }
            return std::nullopt;
        }

        void worker(std::stop_token stopToken, uint32_t queueIndex) {
            m_localQueueIndex = queueIndex;
            m_localQueue = m_localQueues[queueIndex].get();
            m_threadReadyBarrier.count_down();

            while (!stopToken.stop_requested()) {
                if (auto task = m_localQueue->pop()) {
                    std::unique_ptr<task_wrapper> owned(task.value());
                    owned->invoke();
                }
                else if (auto task2 = m_globalQueue.try_pop()) {
                    std::unique_ptr<task_wrapper> owned(task2.value());
                    owned->invoke();
                }
                else if (auto task3 = steal_task()) {
                    std::unique_ptr<task_wrapper> owned(task3.value());
                    owned->invoke();
                }
                else {
                    //put thread to sleep
                }
            }
        }

        mpmc::queue<task_type> m_globalQueue;
        std::vector<std::unique_ptr<concurrent_deque<task_type>>> m_localQueues;

        static inline thread_local concurrent_deque<task_type>* m_localQueue = nullptr;
        static inline thread_local uint32_t m_localQueueIndex = 0;
        std::vector<std::jthread> m_threads;

        std::latch m_threadReadyBarrier;
    };
}
