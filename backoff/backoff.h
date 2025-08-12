#pragma once
#include <algorithm>
#include <cstdint>
#include <thread>

namespace thunder {
    namespace details {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
#include <immintrin.h>
#endif
        inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
            _mm_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
            asm volatile("yield");
#elif defined(__arm__) || defined(_M_ARM)
            asm volatile("yield");
#endif
        }
    }

    class backoff {
    public:
        void reset() noexcept { m_step = 0; }
        void spin() noexcept {
            const uint32_t step = m_step;
            const uint32_t iterations = 1u << std::min(step, m_spin_limit);

            for (uint32_t i = 0; i < iterations; i++) {
                details::cpu_relax();
            }

            if (m_step <= m_spin_limit) {
                m_step++;
            }
        }

        void snooze() noexcept {
            const uint32_t step = m_step;

            if (step <= m_spin_limit) {
                const uint32_t iterations = 1u << step;

                for (uint32_t i = 0; i < iterations; i++) {
                    details::cpu_relax();
                }
            }
            else {
                std::this_thread::yield();
            }

            if (m_step <= m_spin_limit) {
                m_step++;
            }
        }

        [[nodiscard]] bool is_completed() const noexcept { return m_step > m_spin_limit; }
    private:
        static constexpr uint32_t m_spin_limit = 6;
        uint32_t m_step = 0;
    };
}
