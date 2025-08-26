module;
#include <atomic>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <bit>
#include <optional>
export module mpmc_queue;

import backoff;
import hints;

namespace thunder::mpmc {
    struct unique {
        unique() = default;
        ~unique() = default;

        unique(const unique&) = delete;
        unique& operator=(const unique&) = delete;
        unique(unique&&) = delete;
        unique& operator=(unique&&) = delete;
    };

    template<class T>
    class alignas(std::hardware_destructive_interference_size) cell {
        static constexpr bool canCopy = std::is_copy_assignable_v<T>;
        static constexpr bool canMove = std::is_move_assignable_v<T>;
        static constexpr bool preferMove = std::is_nothrow_move_assignable_v<T> || !canCopy;
    public:
        void set_value(T val) noexcept(preferMove || (canCopy && std::is_nothrow_copy_assignable_v<T>)) requires (canCopy || canMove) {
            if constexpr (preferMove) {
                data = std::move(val);
            }
            else {
                data = val;
            }
        }

        template <typename... Args>
        requires std::constructible_from<T, Args...> && std::is_nothrow_constructible_v<T, Args...> && (canCopy || canMove)
        void set_value(Args&&... args) {
            T tmp(std::forward<Args>(args)...);
            if constexpr (preferMove) {
                data = std::move(tmp);
            }
            else {
                data = tmp;
            }
        }

        [[nodiscard]] auto get_value() noexcept(std::is_nothrow_move_constructible_v<T> || std::is_nothrow_copy_constructible_v<T>)
        requires (std::is_move_constructible_v<T> || std::is_copy_constructible_v<T>) {
           return std::move_if_noexcept(data);
        }

        [[nodiscard]] std::size_t load_sequence(const std::memory_order order) const noexcept {
            return sequence.load(order);
        }

        void store_sequence(const std::size_t val, const std::memory_order order) noexcept {
            sequence.store(val, order);
        }
    private:
        T data{};
        std::atomic<std::size_t> sequence{0};
    };

    export enum class spinlock_wait_mode {
        busy_wait,
        backoff_spin,
    };

    export template<class T, spinlock_wait_mode WaitMode = spinlock_wait_mode::busy_wait, class Allocator = std::allocator<cell<T>>>
    class queue : unique {
    public:
        explicit queue(const std::size_t capacity, Allocator allocator = {})
            :
            m_capacity(capacity),
            m_allocator(allocator),
            m_capacityIsPowerOfTwo(std::has_single_bit(m_capacity)),
            m_mask(m_capacity -1),
            m_shift(std::countr_zero(m_capacity))
        {
            if (m_capacity < 1) {
                throw std::invalid_argument("Capacity must be greater than 0");
            }

            m_capacity = std::min(m_capacity,  std::numeric_limits<std::size_t>::max() -1);

            m_buffer = m_allocator.allocate(m_capacity + 1);
            for (size_t i = 0; i < m_capacity; i++) {
                std::construct_at(std::addressof(m_buffer[i]));
            }
        }

        ~queue() noexcept {
            for (size_t i = 0; i < m_capacity; i++) {
                std::destroy_at(std::addressof(m_buffer[i]));
            }
            m_allocator.deallocate(m_buffer, m_capacity + 1);
        }

        template<class U>
        void push(U&& item) {
            emplace(std::forward<U>(item));
        }

        template<class... Args>
        requires std::constructible_from<T, Args...>
        void emplace(Args&&... args) {
            // Relaxed order here is fine: head is just a ticket counter.
            // Ordering is enforced via cell.load_sequence() and cell.store_sequence()
            const auto head = m_head.fetch_add(1, std::memory_order_relaxed);

            std::size_t index; // position within the ring buffer [0, m_capacity)
            std::size_t cycle; // how many full passes of the ring buffer have been completed (monotonic counter of wraparounds)

            // m_capacityIsPowerOfTwo never changes for the lifetime of the object, so this is a
            // perfectly predicted branch, and the cost is thus ~free
            if (m_capacityIsPowerOfTwo) {
                // Avoids div operations when the capacity is a power of two
                index = head & m_mask;
                cycle = head >> m_shift;
            }
            else {
                // Compute index and cycle with a single division:
                // Using both `head / m_capacity` and `head % m_capacity` can lead to TWO hardware divides
                // when `m_capacity` isn’t a compile-time constant.
                // The following code guarantees at most ONE divide; the remainder comes from an inexpensive mul+sub.
                // (Optimizing compilers may fuse / and % automatically, but this form makes it explicit)
                const auto quotient = head / m_capacity;
                index = head - quotient * m_capacity;
                cycle = quotient;
            }

            auto& cell = m_buffer[index];
            const auto sequence = cycle * 2;

            // Wait until the slot is free to write too. std::memory_order_acquire order is needed here
            // because it prevents cell.set_value(...) from being hoisted/reordered before this load.
            // If we used std::memory_order_relaxed, cell.set_value(...) could be reordered above this
            // load and thus introduce a potential write into a slot still owned by the consumer -> data race/UB
            backoff bo;
            while(sequence != cell.load_sequence(std::memory_order_acquire)) {
                if constexpr (WaitMode == spinlock_wait_mode::busy_wait) {
                    hint::spin_loop();
                }
                else {
                    bo.spin();
                }
            }

            cell.set_value(std::forward<Args>(args)...);

            // publish the value. release pairs with the consumer's acquire (publish → consume)
            // std::memory_order_release is needed here so that cell.set_value(...) is not reordered after this store.
            cell.store_sequence(sequence + 1, std::memory_order_release);
        }

        [[nodiscard]] T pop() noexcept(std::is_nothrow_move_constructible_v<T> || std::is_nothrow_copy_constructible_v<T>) {
            // Relaxed order here is fine: tail is just a ticket counter.
            // Ordering is enforced via cell.load_sequence() and cell.store_sequence()
            const auto tail = m_tail.fetch_add(1, std::memory_order_relaxed);

            std::size_t index; // position within the ring buffer [0, m_capacity)
            std::size_t cycle; // how many full passes of the ring buffer have been completed (monotonic counter of wraparounds)

            // m_capacityIsPowerOfTwo never changes for the lifetime of the object, so this is a
            // perfectly predicted branch, and the cost is thus ~free
            if (m_capacityIsPowerOfTwo) {
                // Avoids div operations when the capacity is a power of two
                index = tail & m_mask;
                cycle = tail >> m_shift;
            }
            else {
                // Compute index and cycle with a single division:
                // Using both `tail / m_capacity` and `tail % m_capacity` can lead to TWO hardware divides
                // when `m_capacity` isn’t a compile-time constant.
                // The following code guarantees at most ONE divide; the remainder comes from an inexpensive mul+sub.
                // (Optimizing compilers may fuse / and % automatically, but this form makes it explicit)
                const auto quotient = tail / m_capacity;
                index = tail - quotient * m_capacity;
                cycle = quotient;
            }

            auto& cell = m_buffer[index];
            const auto sequence = cycle * 2 + 1;

            // Wait until the slot has been filled. std::memory_order_acquire establish a happens-before relationship
            // with the producer's release (publish → consume)
            backoff bo;
            while (sequence != cell.load_sequence(std::memory_order_acquire)) {
                if constexpr (WaitMode == spinlock_wait_mode::busy_wait) {
                    hint::spin_loop();
                }
                else {
                    bo.spin();
                }
            }

            // safe to read payload after the acquire above
            auto val = cell.get_value();

            // mark the slot as free. std::memory_order_release is needed to prevent that cell.get_value()
            // is reordered below this store. std::memory_order_release also established a happens-before relationship
            // with the producers acquire
            cell.store_sequence(sequence + 1, std::memory_order_release);
            return val;
        }
    private:
        std::size_t m_capacity;
        [[no_unique_address]] Allocator m_allocator;

        cell<T>* m_buffer{};
        const bool m_capacityIsPowerOfTwo;
        const std::size_t m_mask{}; //only used if capacity is a power of two
        const std::size_t m_shift{}; //only used if capacity is a power of two

        alignas(std::hardware_destructive_interference_size) std::atomic<std::size_t> m_tail{0};
        alignas(std::hardware_destructive_interference_size) std::atomic<std::size_t> m_head{0};
    };
}