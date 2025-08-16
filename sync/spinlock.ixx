module;
#include <atomic>
export module spinlock;

import hints;
import backoff;

namespace thunder {
    export enum class spinlock_wait_mode {
        busy_wait,
        backoff_spin,
    };

    /**
    * @brief A lightweight spinlock for synchronizing access to shared data.
    *
    * The lock can be configured to either busy-wait or spin with exponential backoff
    * while waiting for ownership.
    *
    * @tparam BackoffPolicy Defines the waiting strategy:
    *   - @c spinlock_wait_mode::busy_wait: continuously retries until acquired.
    *   - @c spinlock_wait_mode::backoff_spin: retries with an exponential backoff strategy.
    */
    export template<spinlock_wait_mode BackoffPolicy = spinlock_wait_mode::busy_wait>
    class spinlock {
    public:
        /**
        * @brief Acquires the lock.
        *
        * Blocks the calling thread until the lock becomes available.
        *
        * @exception: This function does not throw any exceptions.
        */
        void lock() noexcept {
            for (;;) {
                if (!m_lock.exchange(true, std::memory_order_acquire)) {
                    return;
                }

                while (m_lock.load(std::memory_order_relaxed)) {
                    if constexpr (BackoffPolicy == spinlock_wait_mode::busy_wait) {
                        hint::spin_loop();
                    }
                    else {
                        m_backoff.spin();
                    }
                }
            }
        }

        /**
        * @brief Attempts to acquire the lock without blocking.
        *
        * @return @c true if the lock was successfully acquired,
        *         @c false if it is already held by another thread.
        *
        * @exception: This function does not throw any exceptions.
        */
        [[nodiscard]] bool try_lock() noexcept {
            return !m_lock.load(std::memory_order_relaxed) && !m_lock.exchange(true, std::memory_order_acquire);
        }

        /**
        * @brief Releases the lock.
        *
        * Makes the lock available for other threads to acquire.
        *
        * @exception: This function does not throw any exceptions.
        */
        void unlock() noexcept {
            m_lock.store(false, std::memory_order_release);
        }

    private:
        std::atomic_bool m_lock = {false};
        backoff m_backoff{};
    };
}
