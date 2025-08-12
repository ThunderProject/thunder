#pragma once
#include <algorithm>
#include <thread>
#include "../utils/hints.h"

namespace thunder {
    class backoff {
    public:
        void reset() noexcept { m_step = 0; }
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

        [[nodiscard]] bool is_completed() const noexcept { return m_step > m_yieldLimit; }
    private:
        static constexpr uint32_t m_spinLimit = 6;
        static constexpr uint32_t m_yieldLimit = 10;
        uint32_t m_step = 0;
    };
}
