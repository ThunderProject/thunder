module;
#include <mutex>
#include <shared_mutex>
#include <type_traits>
#include <utility>
#include <concepts>
#include <functional>
export module guard;

namespace thunder {
    template <class T>
    concept MutexType = requires(T lk) {
        { lk.lock() } -> std::same_as<void>;
        { lk.unlock() } -> std::same_as<void>;
        { lk.try_lock() } -> std::convertible_to<bool>;
    };

    template <class T>
    concept SharedMutexType = MutexType<T> && requires(T lk) {
        { lk.lock_shared() } -> std::same_as<void>;
        { lk.unlock_shared() } -> std::same_as<void>;
        { lk.try_lock_shared() } -> std::convertible_to<bool>;
    };

    template<class T>
    using read_ref_t = std::add_lvalue_reference_t<std::add_const_t<T>>;

    template<class T>
    using write_ref_t = std::add_lvalue_reference_t<T>;

    template<class Ref, class Lock, class T>
    class locked_ptr {
    public:
        using element_type = std::remove_reference_t<Ref>;

        locked_ptr(T* t, Lock&& lk) noexcept(std::is_nothrow_move_constructible_v<Lock>)
            :
            m_ptr{t},
            m_lock{std::move(lk)}
        {}

        locked_ptr(locked_ptr&&) noexcept = default;
        locked_ptr& operator=(locked_ptr&&) noexcept = default;
        locked_ptr(const locked_ptr&) = delete;
        locked_ptr& operator=(const locked_ptr&) = delete;

        Ref get() const noexcept { return *m_ptr; }
        Ref operator*() const noexcept { return *m_ptr; }
        auto operator->() const noexcept { return std::addressof(*m_ptr); }

        auto& lock() noexcept { return m_lock; }
        const auto& lock() const noexcept { return m_lock; }
    private:
        T* m_ptr{};
        Lock m_lock{};
    };

    export template<class T, MutexType Mtx = std::shared_mutex>
    class guard {
    public:
        using value_type = T;
        using mutex_type = Mtx;

        /**
        * @brief Construct the value in-place with the given arguments.
        *
        * @param args Arguments forwarded to the constructor of value_type.
        */
        template <class... Args>
        explicit guard(std::in_place_t, Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>)
            :
            m_value(std::forward<Args>(args)...)
        {}

        /// Default-construct the value
        guard() requires std::default_initializable<T> = default;

        guard(const guard&) = delete;
        guard& operator=(const guard&) = delete;

        guard(guard&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
            :
            m_value(std::move(other.m_value))
        {}

        guard& operator=(guard&& other) noexcept(std::is_nothrow_move_assignable_v<T>) {
            if (this != &other) {
                std::scoped_lock lk(m_mutex, other.m_mutex);
                m_value = std::move(other.m_value);
            }
            return *this;
        }

        /**
         * @brief Acquire an exclusive lock for modifying the value.
         *
         * @return RAII pointer granting write access to the value.
         */
        auto lock() noexcept {
            std::unique_lock<mutex_type> lk(m_mutex);
            return locked_ptr<write_ref_t<T>, decltype(lk), T>{std::addressof(m_value), std::move(lk)};
        }

        /**
        * @brief Acquire an exclusive lock for reading the value.
        *
        * @return RAII pointer granting read-only access to the value.
        */
        auto lock() const noexcept {
            std::unique_lock<mutex_type> lk(m_mutex);
            return locked_ptr<read_ref_t<T>, decltype(lk), const T>{std::addressof(m_value), std::move(lk)};
        }

        /**
         * @brief Acquire a shared lock for reading the value.
         *
         * Available only if mutex_type supports shared locking.
         *
         * @return RAII pointer granting read-only access to the value.
         */
        auto read() noexcept requires SharedMutexType<mutex_type> {
            std::shared_lock<mutex_type> lk(m_mutex);
            return locked_ptr<read_ref_t<T>, decltype(lk), T>{std::addressof(m_value), std::move(lk)};
        }

        /**
         * @brief Acquire a shared lock for reading the value.
         *
         * Available only if mutex_type supports shared locking.
         *
         * @return RAII pointer granting read-only access to the value.
         */
        auto read() const noexcept requires SharedMutexType<mutex_type> {
            std::shared_lock<mutex_type> lk(m_mutex);
            return locked_ptr<read_ref_t<T>, decltype(lk), const T>{std::addressof(m_value), std::move(lk)};
        }

        /**
         * @brief Acquire an exclusive lock for writing the value.
         *
         * Alias for lock() when shared locks are supported.
         *
         * @return RAII pointer granting write access to the value.
         */
        auto write() noexcept requires SharedMutexType<mutex_type> { return lock(); }

        template <class F>
        decltype(auto) with_lock(F&& f) {
            auto lk = lock();
            return std::invoke(std::forward<F>(f), lk.get());
        }

        template <class F>
        decltype(auto) with_lock(F&& f) const {
            auto lk = lock();
            return std::invoke(std::forward<F>(f), lk.get());
        }

        template <class F>
        decltype(auto) with_read_lock(F&& f) requires SharedMutexType<mutex_type> {
            auto reader = read();
            return std::invoke(std::forward<F>(f), reader.get());
        }

        template <class F>
        decltype(auto) with_read_lock(F&& f) const requires SharedMutexType<mutex_type> {
            auto reader = read();
            return std::invoke(std::forward<F>(f), reader.get());
        }

        template <class F>
        decltype(auto) with_write_lock(F&& f) requires SharedMutexType<mutex_type> {
            auto writer = write();
            return std::invoke(std::forward<F>(f), writer.get());
        }

        mutex_type& native_handle() noexcept { return m_mutex; }
        const mutex_type& native_handle() const noexcept { return m_mutex; }
    private:
        mutable mutex_type m_mutex{};
        value_type m_value{};
    };
}