#pragma once

namespace thunder::hint {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
#include <immintrin.h>
#endif
    inline void spin_loop() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
        _mm_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
        asm volatile("yield");
#elif defined(__arm__) || defined(_M_ARM)
        asm volatile("yield");
#endif
    }
}
