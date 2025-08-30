module;
#include <coroutine>
#include <exception>
export module sync_wait;

import completion_event;

namespace thunder::coro {
    export struct sync_wait_promise;
    export struct sync_wait_task {
        using promise_type = sync_wait_promise;

        explicit sync_wait_task(const std::coroutine_handle<promise_type> coro) noexcept
            :
            m_coroHandle(coro)
        {}

        ~sync_wait_task() noexcept {
            if(m_coroHandle) {
                m_coroHandle.destroy();
            }
        }

        void run(completion_event& event) const;
    private:
        std::coroutine_handle<sync_wait_promise> m_coroHandle;
    };

    export struct sync_wait_promise {
        static std::suspend_always initial_suspend() noexcept { return {}; }

        static auto final_suspend() noexcept {
            struct awaiter {
                static auto await_ready() noexcept { return false; }

                static void await_suspend(std::coroutine_handle<sync_wait_promise> coroHandle) noexcept {
                    if(const auto completionEvent = coroHandle.promise().m_event) {
                        completionEvent->set();
                    }
                }

                static void await_resume() noexcept {}
            };

            return awaiter();
        }

        static void return_void() noexcept {}

        sync_wait_task get_return_object() noexcept {
            return sync_wait_task{ std::coroutine_handle<sync_wait_promise>::from_promise(*this) };
        }

        static void unhandled_exception() noexcept { std::terminate(); }

        completion_event* m_event {nullptr};
    };

    void sync_wait_task::run(completion_event &event) const {
        m_coroHandle.promise().m_event = &event;
        m_coroHandle.resume();
    }
}
