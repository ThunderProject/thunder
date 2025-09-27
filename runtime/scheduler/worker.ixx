module;
#include <thread>
#include <libassert/assert.hpp>
export module worker;
import backoff;

namespace thunder::cpu {
    class scheduler;
    class worker {
    public:
        worker(scheduler& scheduler);
        void run(std::stop_token stopToken, uint32_t queueIndex) {}
    private:
        scheduler& m_handle;
    };
}
