module;
#include <vector>
#include <libassert/assert.hpp>
export module reclaimer;

namespace thunder {
    template<class T>
    concept buffer_type = !std::is_array_v<T>;

    export template<buffer_type buffer>
    class bounded_reclaimer {
    public:
        void collect(buffer* buf) noexcept {}
    };

    export template<buffer_type buffer>
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
            // Collecting a nullptr buffer is technically not an error because
            // we never access the pointed to object, we only delete it once the destructor runs,
            // and deleting a nullptr is well-defined by the standard. However, it does not make sense to
            // collect a nullptr, so we are probably using this utility incorrectly if that happens, so lets catch
            // that behavior in debug mode.
            DEBUG_ASSERT(buf != nullptr);
            m_garbage.emplace_back(buf);
        }
    private:
        std::vector<buffer*> m_garbage;
    };
}
