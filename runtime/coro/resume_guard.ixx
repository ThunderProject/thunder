module;
#include <coroutine>
export module resume_guard;

namespace thunder::coro {
    export struct resume_guard {
        explicit resume_guard(std::coroutine_handle<> handle) noexcept
            :
            m_handle(handle)
        {
            if (m_handle) {
                m_handle.resume();
            }
        }

        resume_guard(const resume_guard& rhs) = delete;
        resume_guard& operator=(const resume_guard& rhs) = delete;

        ~resume_guard() noexcept {
            if (m_handle) {
                if(m_handle.done()) {
                    m_handle.destroy();
                }
            }
        }
    private:
        std::coroutine_handle<> m_handle{nullptr};
    };
}