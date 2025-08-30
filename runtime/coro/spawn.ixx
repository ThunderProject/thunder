module;
#include <exception>
#include <coroutine>
export module spawn;

import task;

namespace thunder::coro {
    export struct spawn_task {
        struct promise_type final {
            static auto get_return_object() noexcept { return spawn_task{}; }
            static auto initial_suspend() noexcept { return std::suspend_never{}; }
            static auto final_suspend() noexcept { return std::suspend_never{}; }
            static void return_void() noexcept {}
            static void unhandled_exception() noexcept(false) { std::terminate(); }
        };
    };

    export template<class T>
    spawn_task spawn(task<T> awaitable) {
        co_await awaitable;
    }
}
