module;
#include <atomic>
export module completion_event;

namespace thunder::coro {
    export class completion_event {
    public:
        void set() noexcept {
            m_flag.test_and_set(std::memory_order_seq_cst);
            m_flag.notify_all();
        }

        void wait() const noexcept {
            m_flag.wait(false, std::memory_order_seq_cst);
        }
    private:
        std::atomic_flag m_flag;
    };
}
