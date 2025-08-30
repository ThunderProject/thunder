module;
#include <coroutine>
#include <exception>
#include <utility>
export module task;

import completion_event;
import sync_wait;

namespace thunder::coro {
    export  template<class T>
    struct task_promise;

    export template<class T = void>
    class task {
    public:
        using promise_type = task_promise<T>;

        explicit task(std::coroutine_handle<promise_type> handle) noexcept
            :
            m_coroHandle(handle)
        {}

        auto operator co_await() const noexcept {
            struct awaiter {
                [[nodiscard]] bool await_ready() noexcept {
                    return !m_coroHandle || m_coroHandle.done();
                }

                std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaitingCoro) noexcept {
                    m_coroHandle.promise().set_continuation(awaitingCoro);
                    return m_coroHandle;
                }

                auto await_resume() noexcept {
                    if constexpr (!std::is_void_v<T>) {
                        auto& promise = m_coroHandle.promise();
                        auto returnValue = std::move(promise.value);

                        auto ownedHandle = std::exchange(m_coroHandle, nullptr);
                        ownedHandle.destroy();
                        return returnValue;
                    }
                    else {
                        auto ownedHandle = std::exchange(m_coroHandle, nullptr);
                        ownedHandle.destroy();
                    }
                }

                std::coroutine_handle<promise_type> m_coroHandle{ nullptr };
            };

            return awaiter{ m_coroHandle };
        }

        auto result() noexcept requires(!std::is_void_v<T>) { return m_coroHandle.promise().value; }
        void result() const noexcept requires(std::is_void_v<T>) {}

        void wait() {
            completion_event event;
            const auto waitTask = create_sync_wait_task();
            waitTask.run(event);
            event.wait();
        }

        T get() requires(!std::is_void_v<T>) {
            wait();
            return result();
        }

        void get() requires(std::is_void_v<T>) { wait(); }
    private:
        sync_wait_task create_sync_wait_task() {
            co_await *this;
        }

        std::coroutine_handle<promise_type> m_coroHandle{};
    };

    export template<class T>
    struct task_promise {
        struct finalize_awaitable {
            static auto await_ready() noexcept { return false; }
            auto await_suspend(std::coroutine_handle<task_promise> coro) noexcept { return coro.promise().m_continuation; }
            static void await_resume() noexcept {}
        };

        task<T> get_return_object() noexcept {
            return task<T>{ std::coroutine_handle<task_promise>::from_promise(*this) };
        }

        static auto initial_suspend() noexcept { return std::suspend_always {}; }
        auto final_suspend() const noexcept { return finalize_awaitable(); }

        void return_value(T value) noexcept { this->value = value; }
        static void unhandled_exception() noexcept { std::terminate(); }

        void set_continuation(std::coroutine_handle<> continuation) noexcept { m_continuation = continuation; }

        T value{};
    private:
        std::coroutine_handle<> m_continuation = std::noop_coroutine();
    };

    export template<>
    struct task_promise<void> {
        struct final_awaitable {
            static auto await_ready() noexcept { return false; }

            static std::coroutine_handle<> await_suspend(std::coroutine_handle<task_promise> coro) noexcept {
                if (coro.promise().m_continuation) {
                    return coro.promise().m_continuation;
                }
                return std::noop_coroutine();
            }

            static void await_resume() noexcept {}
        };

        auto get_return_object() noexcept {
            return task{ std::coroutine_handle<task_promise>::from_promise(*this) };
        }

        [[nodiscard]] static auto initial_suspend() noexcept { return std::suspend_always{}; }
        [[nodiscard]] static auto final_suspend() noexcept { return final_awaitable{}; }

        static void return_void() noexcept {}
        void unhandled_exception() noexcept { m_exceptionPtr = std::current_exception(); }

        void set_continuation(std::coroutine_handle<> continuation) noexcept { m_continuation = continuation; }
    private:
        std::coroutine_handle<> m_continuation{ nullptr };
        std::exception_ptr m_exceptionPtr{nullptr};
    };
}
