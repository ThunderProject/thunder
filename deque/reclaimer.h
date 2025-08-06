#pragma once
#include <vector>

namespace thunder {
    template<class T>
    concept buffer_type = !std::is_array_v<T>;

    template<buffer_type buffer>
    class null_reclaimer {
    public:
        void collect(buffer* buf) noexcept {}
    };

    template<buffer_type buffer>
    class bounded_reclaimer {
    public:
        void collect(buffer* buf) noexcept {}
    };

    template<buffer_type buffer>
    class deferred_reclaimer {
    public:
        deferred_reclaimer() noexcept {
            m_garbage.reserve(64);
        }

        ~deferred_reclaimer() noexcept {
            for (auto& buf : m_garbage) {
                delete buf;
            }
        }

        void collect(buffer* buf) noexcept {
            m_garbage.emplace_back(buf);
        }
    private:
        std::vector<buffer*> m_garbage;
    };
}
