module;
//windows.h is such a stupid header
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define STRICT
#define UNICODE

#include <windows.h>
#endif
#include <thread>
#include <string>

export module thunder_thread;

namespace thunder {
    namespace windows {
        void set_thread_name(const std::string_view name, HANDLE threadHandle) noexcept {
#ifdef _WIN32
            const auto length = MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
            if (length > 0) {
                std::wstring wideName(length, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), wideName.data(), length);
                [[maybe_unused]] auto result = SetThreadDescription(threadHandle, wideName.c_str());
            }
#endif
        }
    }

    export class thunder_thread {
    public:
        using id = std::jthread::id;
        using native_handle_type = std::jthread::native_handle_type;

        thunder_thread() noexcept = default;

        template <class Fn, class... Args>
        requires (!std::is_same_v<std::remove_cvref_t<Fn>, thunder_thread>)
        [[nodiscard]] explicit thunder_thread(Fn&& fn, Args&&... args)
            :
            m_thread(std::forward<Fn>(fn), std::forward<Args>(args)...)
        {}

        ~thunder_thread() { try_cancel_and_join(); }

        thunder_thread(const thunder_thread&) = delete;
        thunder_thread(thunder_thread&&) noexcept = default;
        thunder_thread& operator=(const thunder_thread&) = delete;

        thunder_thread& operator=(thunder_thread&& rhs) noexcept {
            if (this == std::addressof(rhs)) {
                return *this;
            }

            try_cancel_and_join();
            m_thread = std::move(rhs.m_thread);
            return *this;
        }

        void swap(thunder_thread& rhs) noexcept { m_thread.swap(rhs.m_thread); }
        [[nodiscard]] bool joinable() const noexcept { return m_thread.joinable(); }
        void join() { m_thread.join(); }
        void detach() { m_thread.detach(); }
        [[nodiscard]] id get_id() const noexcept { return m_thread.get_id(); }
        [[nodiscard]] native_handle_type native_handle() noexcept { return m_thread.native_handle(); }
        [[nodiscard]] std::stop_source get_stop_source() noexcept { return m_thread.get_stop_source(); }
        [[nodiscard]] std::stop_token get_stop_token() const noexcept { return m_thread.get_stop_token(); }
        bool request_stop() noexcept { return m_thread.request_stop(); }

        void set_thread_name(const std::string_view name) noexcept {
#ifdef _WIN32
            windows::set_thread_name(name, m_thread.native_handle());
#endif
        }

        friend void swap(thunder_thread& lhs, thunder_thread& rhs) noexcept {
            lhs.m_thread.swap(rhs.m_thread);
        }

        [[nodiscard]] static unsigned int hardware_concurrency() noexcept { return std::thread::hardware_concurrency(); }
    private:
        void try_cancel_and_join() noexcept {
            if (m_thread.joinable()) {
                m_thread.request_stop();
                m_thread.join();
            }
        }
        std::jthread m_thread;
    };
}
