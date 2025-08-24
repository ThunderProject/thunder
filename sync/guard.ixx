module;
#include <mutex>
#include <shared_mutex>
#include <type_traits>
#include <utility>
#include <concepts>
#include <functional>
#include <libassert/assert.hpp>
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

    /**
    * @brief RAII-pointer-like wrapper that couples a datum with a held lock.
    *
    * @tparam Reference The reference type exposed to the user (e.g., \c T& or \c const& T).
    * @tparam Lock  The lock type.
    * @tparam Datum The underlying stored datum type.
    *
    * A \c locked_ptr behaves like a pointer to the Datum while ensuring the associated Lock
    * remains held for the lifetime of the \c locked_ptr. It is movable but not copyable.
    */
    export template<class Reference, class Lock, class Datum>
    class locked_ptr {
    public:
        /**
        * @brief Construct from a raw pointer and an already-acquired lock.
        *
        * @param datum Pointer to the protected datum.
        * @param lk A lock object owning the lock. Moved into the \c locked_ptr.
        */
        locked_ptr(Datum* datum, Lock&& lk) noexcept(std::is_nothrow_move_constructible_v<Lock>)
            :
            m_datum{datum},
            m_lock{std::move(lk)}
        {}

        /**
        * @brief Move-construct. Transfers pointer and lock ownership.
        */
        locked_ptr(locked_ptr&&) noexcept = default;

        /**
        * @brief Move-assign. Transfers pointer and lock ownership.
        */

        locked_ptr& operator=(locked_ptr&&) noexcept = default;

        locked_ptr(const locked_ptr&) = delete;
        locked_ptr& operator=(const locked_ptr&) = delete;

        /**
         * @brief Access the referenced datum.
         * @return The stored reference (const or mutable according to \c Reference).
        */
        Reference get() const noexcept {
            DEBUG_ASSERT(m_datum != nullptr);
            return *m_datum;
        }

        /**
         * @brief Access the referenced datum.
         * @return The stored reference (const or mutable according to \c Reference).
        */
        Reference operator*() const noexcept {
            DEBUG_ASSERT(m_datum != nullptr);
            return *m_datum;
        }

        /**
         * @brief Member-access operator.
         * @return Pointer to the datum suitable for member access.
         */
        auto operator->() const noexcept {
            DEBUG_ASSERT(m_datum != nullptr);
            return std::addressof(*m_datum);
        }

        /**
        * @brief Access the underlying lock.
        * @return Reference to the lock object.
        * @note Use with caution
        */
        auto& get_lock() noexcept { return m_lock; }

        /**
        * @brief Access the underlying lock.
        * @return Reference to the lock object.
        * @note Use with caution
        */
        const auto& get_lock() const noexcept { return m_lock; }
    private:
        Datum* m_datum{};
        Lock m_lock{};
    };

    /**
    * @brief Thread-safe guard around a datum with mutex-based synchronization.
    *
    * @tparam Datum Stored datum type.
    * @tparam Mtx Mutex type used for synchronization (defaults to \c std::shared_mutex).
    *
    * Provides convenient APIs to acquire exclusive or shared access to the contained value.
    * Access is exposed through \c locked_ptr, which keeps the lock held until it goes out of scope,
    * then it automatically releases it.
    * Additionally, utility helpers \c with_lock execute a callable
    * while holding the appropriate lock and return its result.
    */
    export template<class Datum, MutexType Mtx = std::shared_mutex>
    class guard {
    public:
        /**
        * @brief Construct the value in-place with the given arguments.
        *
        * @param args Arguments forwarded to the constructor of value_type.
        */
        template <class... Args>
        explicit guard(std::in_place_t, Args&&... args) noexcept(std::is_nothrow_constructible_v<Datum, Args...>)
            :
            m_datum(std::forward<Args>(args)...)
        {}

        /**
        * @brief Default-construct the value. Enabled only if \c Datum is default initializable
        */
        guard() requires std::default_initializable<Datum> = default;

        guard(const guard&) = delete;
        guard& operator=(const guard&) = delete;

        /**
         * @brief Move-construct the guard and its Datum.
         */
        guard(guard&& other) noexcept(std::is_nothrow_move_constructible_v<Datum>)
            :
            m_datum(std::move(other.m_datum))
        {}

        /**
        * @brief Move-assign the guard and its Datum.
        */
        guard& operator=(guard&& other) noexcept(std::is_nothrow_move_assignable_v<Datum>) {
            if (this != &other) {
                std::scoped_lock lk(m_mutex, other.m_mutex);
                m_datum = std::move(other.m_datum);
            }
            return *this;
        }

        /**
         * @brief Acquire an exclusive lock for modifying the value.
         *
         * @return RAII pointer granting write access to the value.
         */
        auto lock() noexcept {
            std::unique_lock<Mtx> lk(m_mutex);
            return locked_ptr<write_ref_t<Datum>, decltype(lk), Datum>{std::addressof(m_datum), std::move(lk)};
        }

        /**
        * @brief Acquire an exclusive lock for reading the value.
        *
        * @return RAII pointer granting read-only access to the value.
        */
        auto lock() const noexcept {
            std::unique_lock<Mtx> lk(m_mutex);
            return locked_ptr<read_ref_t<Datum>, decltype(lk), const Datum>{std::addressof(m_datum), std::move(lk)};
        }

        /**
         * @brief Acquire a shared lock for reading the value.
         *
         * Available only if \c Mtx supports shared locking.
         *
         * @return RAII pointer granting read-only access to the value.
         */
        auto read() noexcept requires SharedMutexType<Mtx> {
            std::shared_lock<Mtx> lk(m_mutex);
            return locked_ptr<read_ref_t<Datum>, decltype(lk), Datum>{std::addressof(m_datum), std::move(lk)};
        }

        /**
         * @brief Acquire a shared lock for reading the value.
         *
         * Available only if \ Mtx supports shared locking.
         *
         * @return RAII pointer granting read-only access to the value.
         */
        auto read() const noexcept requires SharedMutexType<Mtx> {
            std::shared_lock<Mtx> lk(m_mutex);
            return locked_ptr<read_ref_t<Datum>, decltype(lk), const Datum>{std::addressof(m_datum), std::move(lk)};
        }

        /**
         * @brief Acquire an exclusive lock for writing the value.
         *
         * Alias for lock() when shared locks are supported.
         *
         * @return RAII pointer granting write access to the value.
         */
        auto write() noexcept requires SharedMutexType<Mtx> { return lock(); }

        /**
         * @brief Execute a callable while holding an exclusive lock.
         *
         * @tparam Callable Callable taking \c Datum& and returning any type.
         * @param callable  The callable to invoke.
         * @return Whatever the callable returns.
         */
        template <class Callable>
        decltype(auto) with_lock(Callable&& callable) {
            auto lk = lock();
            return std::invoke(std::forward<Callable>(callable), lk.get());
        }

        /**
         * @brief Execute a callable while holding an exclusive lock.
         *
         * @tparam Callable Callable taking \code const Datum& \endcode and returning any type.
         * @param callable  The callable to invoke.
         * @return Whatever the callable returns.
         */
        template <class Callable>
        decltype(auto) with_lock(Callable&& callable) const {
            auto lk = lock();
            return std::invoke(std::forward<Callable>(callable), lk.get());
        }

        /**
         * @brief Execute a callable while holding a shared read lock.
         *
         * Enabled only if shared locks are supported.
         *
         * @tparam Callable Callable taking \code const Datum& \endcode and returning any type.
         * @param callable  The callable to invoke.
         * @return Whatever the callable returns.
         */
        template <class Callable>
        decltype(auto) with_read_lock(Callable&& callable) requires SharedMutexType<Mtx> {
            auto reader = read();
            return std::invoke(std::forward<Callable>(callable), reader.get());
        }

        /**
        * @brief Execute a callable while holding a shared read lock.
        *
        * Enabled only if shared locks are supported.
        *
        * @tparam Callable Callable taking \code const Datum& \endcode and returning any type.
        * @param callable  The callable to invoke.
        * @return Whatever the callable returns.
        */
        template <class Callable>
        decltype(auto) with_read_lock(Callable&& callable) const requires SharedMutexType<Mtx> {
            auto reader = read();
            return std::invoke(std::forward<Callable>(callable), reader.get());
        }

        /**
         * @brief Execute a callable while holding an exclusive write lock.
         *
         * Enabled only if shared locks are supported.
         *
         * @tparam Callable Callable taking \code T& Datum \endcode and returning any type.
         * @param callable  The callable to invoke.
         * @return Whatever the callable returns.
         */
        template <class Callable>
        decltype(auto) with_write_lock(Callable&& callable) requires SharedMutexType<Mtx> {
            auto writer = write();
            return std::invoke(std::forward<Callable>(callable), writer.get());
        }

        /**
        * @brief Access the underlying mutex.
        * @return Reference to the mutex used by this guard.
        */
        Mtx& native_handle() noexcept { return m_mutex; }

        /**
        * @brief Access the underlying mutex.
        * @return Reference to the mutex used by this guard.
        */
        const Mtx& native_handle() const noexcept { return m_mutex; }
    private:
        mutable Mtx m_mutex{};
        Datum m_datum{};
    };
}