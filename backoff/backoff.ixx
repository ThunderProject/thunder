export module backoff;

#include <algorithm>
#include <thread>

import hints;

namespace thunder {
    /**
    * @brief Performs exponential backoff in spin loop.
    *
    * Backing off in spin loops reduces contention and improves overall performance.
    *
    * This primitive can execute YIELD and PAUSE instructions, yield the current thread to the OS scheduler,
    * and tell when it is a good time to block the thread using a different synchronization mechanism.
    *
    */
    export class backoff {
    public:
        /**
        * @brief Resets the backoff.
        *
        * After calling this, the next `spin()` or `snooze()` call will start
        * again from the shortest delay.
        */
        void reset() noexcept { m_step = 0; }

        /**
        * @brief Backs off in a lock-free loop.
        *
        * This method should be used when we need to retry an operation because another thread made progress.
        *
        * The processor may yield using the YIELD or PAUSE instruction
        */
        void spin() noexcept {
            const uint32_t step = m_step;
            const uint32_t iterations = 1u << std::min(step, m_spinLimit);

            for (uint32_t i = 0; i < iterations; i++) {
                hint::spin_loop();
            }

            if (m_step <= m_spinLimit) {
                m_step++;
            }
        }

        /**
         * @brief Backs off in a blocking loop.
         *
         * This method should be used when we need to wait for another thread to make progress.
         *
         * The processor may yield using the YIELD or PAUSE instruction,
         * and the current thread may yield by giving up a timeslice to the OS scheduler.
         *
         * If possible, use \c is_completed to check when it is advised to stop using backoff and block
         * the current thread using a different synchronization mechanism instead.
         */
        void snooze() noexcept {
            const uint32_t step = m_step;

            if (step <= m_spinLimit) {
                const uint32_t iterations = 1u << step;

                for (uint32_t i = 0; i < iterations; i++) {
                    hint::spin_loop();
                }
            }
            else {
                std::this_thread::yield();
            }

            if (m_step <= m_yieldLimit) {
                m_step++;
            }
        }

        /**
        * @brief Check if exponential backoff has completed.
        *
        * @return \c true if exponential backoff has completed and blocking the thread is advised otherwise false
        */
        [[nodiscard]] bool is_completed() const noexcept { return m_step > m_yieldLimit; }
    private:
        static constexpr uint32_t m_spinLimit = 6;
        static constexpr uint32_t m_yieldLimit = 10;
        uint32_t m_step = 0;
    };
}
