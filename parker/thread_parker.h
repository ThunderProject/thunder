#pragma once
#include <semaphore>
#include <chrono>

namespace thunder {
    namespace details {
        template<class Rep, class Period>
        concept is_duration = requires
        {
            typename std::chrono::duration<Rep, Period>;
            requires std::is_arithmetic_v<Rep>;
        };
    }

     /**
     * @brief Lightweight utility for blocking and waking threads.
     *
     * A parked thread remains blocked until it is explicitly unparked from
     * another thread, or an optional timeout elapses.
     *
     * Typical usage:
     * - A thread calls `park()` (or one of the timed variants) to wait until
     *   it should resume execution.
     * - Another thread calls `unpark()` to wake the parked thread.
     *
     * This is often used in synchronization primitives or task schedulers to
     * efficiently block idle threads until new work is available.
     */
    class thread_parker {
    public:
        thread_parker()
            :
            m_token(0)
        {}

        /**
        * @brief Parks the calling thread until a token is available.
        *
        * Blocks unless or until the token has been made available via `unpark()`.
        *
        * `unpark()` followed by `park()` guarantees that this call returns immediately,
        *
        * Memory Ordering:
        * - `unpark()` synchronizes-with this `park()`,
        * guaranteeing that memory operations before `unpark()` are visible after `park()`.
        *
        * @throws std::system_error if the underlying token operation fails.
        */
        void park() {
            // Fast path: try to consume the token without blocking
            if (m_token.try_acquire()) {
                return;
            }
            // block until unpark is called
            m_token.acquire();
        }

        /**
        * @brief Parks the calling thread for up to the specified duration.
        *
        * If a token is available, it is consumed immediately and `true` is returned.
        * If no token is available and timeout is zero or negative, returns `false` without blocking.
        * Otherwise, blocks until `unpark()` is called or the timeout duration has been exceeded.
        *
        * Memory Ordering:
        * - `unpark()` synchronizes-with this `park_for()`,
        * guaranteeing that memory operations before `unpark()` are visible after `park_for()`.
        *
        * @return `true` if a token was acquired, `false` on timeout.
        *
        * @throws std::system_error or a timeout-related exception if the underlying operations fails.
        */
        template<class Rep, class Period>
        requires details::is_duration<Rep, Period>
        [[nodiscard]] bool park_for(const std::chrono::duration<Rep, Period>& timeout) {
            // Fast path: try to consume the token without blocking
            if (m_token.try_acquire()) {
                return true;
            }

            // If the timeout is zero or negative, then there is no need to actually block.
            if (timeout <= std::chrono::duration<Rep, Period>::zero()) {
                return false;
            }

            // block until unpark is called or the timeout duration elapsed
            return m_token.try_acquire_for(timeout);
        }

        /**
        * @brief Parks the calling thread until a token is available, but only up until the specified time point.
        *
        * If a token is available, it is consumed immediately and `true` is returned.
        * Otherwise, blocks until `unpark()` is called or the time point is reached.
        *
        * Memory Ordering:
        * - `unpark()` synchronizes-with this `park_until()`, guaranteeing that memory
        *   operations before `unpark()` are visible after `park_until()` returns `true`.
        *
        * @return `true` if a token was acquired, `false` on timeout.
        *
        * @throws std::system_error or a timeout-related exception if the underlying operations fails.
        */
        template<class Clock, class Duration>
        requires std::chrono::is_clock_v<Clock>
        [[nodiscard]] bool park_until(const std::chrono::time_point<Clock, Duration>& timepoint) {
            // Fast path: try to consume the token without blocking
            if (m_token.try_acquire()) {
                return true;
            }

            // block until unpark is called or the timepoint has been passed
            return m_token.try_acquire_until(timepoint);
        }

        /**
        * @brief Atomically makes the token available if it is not already
        *
        * This method will wake up the thread blocked on`park` or `park_for`, if there is any
        *
        * This operation strongly happens-before invocations of `acquire()` or `try_acquire()` that observe its effects.
        */
        void unpark() {
            // Atomically increments the internal counter by the value of 1. Any thread(s) waiting for the counter to be greater than 0,
            // such as due to being blocked in acquire, will subsequently be unblocked.
            //This operation strongly happens before invocations of try_acquire that observe the result of the effects.
            m_token.release();
        }
    private:
        std::binary_semaphore m_token;
    };
}