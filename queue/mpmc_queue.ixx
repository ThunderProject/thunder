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
        cell() = default;
        ~cell() noexcept {
            if (sequence.load(std::memory_order_seq_cst) & 1) {
                data.~T();
            }
        }

        void set_value(T val) noexcept requires (canCopy || canMove) {
            if constexpr (preferMove) {
                data = std::move(val);
            }
            else {
                data = val;
            }
        }

        template <typename... Args>
        requires canCopy || canMove
        void set_value(Args&&... args) noexcept {
            T tmp(std::forward<Args>(args)...);
            if constexpr (preferMove) {
                data = std::move(tmp);
            }
            else {
                data = tmp;
            }
        }

        [[nodiscard]] auto get_value() noexcept requires (std::is_move_constructible_v<T> || std::is_copy_constructible_v<T>) {
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

    export enum class wait_mode {
        busy_wait, // Pure spin-waiting, lowest latency but can waste CPU cycles.
        backoff_spin, // spinning with exponential backoff to reduce contention.
    };

    struct ticket {
        std::size_t index; // position within the ring buffer [0, m_capacity)
        std::size_t cycle; // how many full passes of the ring buffer have been completed (monotonic counter of wraparounds)
    };

    class ticker_dispenser {
    public:
        explicit ticker_dispenser(std::size_t capacity) noexcept
            :
            m_capacity(capacity),
            m_pow2(std::has_single_bit(capacity)),
            m_mask(capacity - 1),
            m_shift(std::countr_zero(capacity))
        {}

        [[nodiscard]] ticket next_producer() noexcept {
            // Relaxed order here is fine: head is just a ticket counter.
            // Ordering is enforced by the cell class
            return compute_ticket(m_head.fetch_add(1, std::memory_order_relaxed));
        }

        [[nodiscard]] ticket next_consumer() noexcept {
            // Relaxed order here is fine: head is just a ticket counter.
            // Ordering is enforced by the cell class
            return compute_ticket(m_tail.fetch_add(1, std::memory_order_relaxed));
        }

        [[nodiscard]] ticket compute_ticket(const std::size_t index) const noexcept {
            // m_pow2 never changes for the lifetime of the object, so this is a
            // perfectly predicted branch, and the cost is thus ~free
            if (m_pow2) {
                // Avoids div operations when the capacity is a power of two
                return ticket {
                    .index = index & m_mask,
                    .cycle = index >> m_shift,
                };
            }

            // Compute index and cycle with a single division:
            // Using both `tail / m_capacity` and `tail % m_capacity` can lead to TWO hardware divides
            // when `m_capacity` isn’t a compile-time constant.
            // The following code guarantees at most ONE divide; the remainder comes from an inexpensive mul+sub.
            // (Optimizing compilers may fuse / and % automatically, but this form makes it explicit)
            const auto quotient = index / m_capacity;
            return {
                .index = index - quotient * m_capacity,
                .cycle = quotient
            };
        }

        [[nodiscard]]std::atomic<std::size_t>& head() noexcept { return m_head; }
        [[nodiscard]]std::atomic<std::size_t>& tail() noexcept { return m_tail; }

        [[nodiscard]] std::size_t load_head() const noexcept { return m_head.load(std::memory_order_relaxed); }
        [[nodiscard]] std::size_t load_tail() const noexcept { return m_tail.load(std::memory_order_relaxed); }
    private:

        const std::size_t m_capacity;
        const bool m_pow2;
        const std::size_t m_mask; //only used if capacity is a power of two
        const std::size_t m_shift; //only used if capacity is a power of two

        alignas(std::hardware_destructive_interference_size) std::atomic<std::size_t> m_head{0};
        alignas(std::hardware_destructive_interference_size) std::atomic<std::size_t> m_tail{0};
    };

    /**
    * @brief multi-producer, multi-consumer lock-free queue.
    *
    * This queue supports multiple producer threads (calling \c push / \c try_push / \c emplace)
    * and multiple consumer threads (calling \c pop / \c try_pop).
    * A thread may also freely switch between being a producer and consumer
    * @tparam T         Item type.
    * @tparam WaitMode  The WaitMode to use
    * @tparam Allocator Allocator type for storage.
    */
    export template<class T, wait_mode WaitMode = wait_mode::backoff_spin, class Allocator = std::allocator<cell<T>>>
    class queue : unique {
    public:
        /**
        * @brief Construct a queue with a given logical capacity.
        * @param capacity   Maximum number of elements that can be stored concurrently.
        * @param allocator  Allocator instance for the underlying storage.
        */
        explicit queue(const std::size_t capacity, Allocator allocator = {})
            :
            m_capacity(capacity),
            m_allocator(allocator),
            m_ticket_dispenser(capacity)
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

        /**
        * @brief Push an item into the queue.
        * @param item The item to push into the queue.
        * @note Blocks if the queue is full until space is available.
        */
        template<class U>
        void push(U&& item) noexcept { emplace(std::forward<U>(item)); }

        /**
        * @brief Tries to push an item into the queue without blocking.
        * @param item The item to push into the queue.
        * @return \c true if the item was enqueued, otherwise \c false.
        */
        template<class U>
        [[nodiscard]] bool try_push(U&& item) noexcept { return try_emplace(std::forward<U>(item)); }

        /**
        * @brief In-place construct and push the item into the queue.
        * @tparam Args Constructor argument types for \c T.
        * @param args  Arguments forwarded to \c T's constructor.
        * @note Blocks if the queue is full until space is available.
        */
        template<class... Args>
        requires std::constructible_from<T, Args...> && std::is_nothrow_constructible_v<T, Args...>
        void emplace(Args&&... args) noexcept {
            const auto [index, cycle] = m_ticket_dispenser.next_producer();

            auto& cell = m_buffer[index];
            const auto sequence = cycle * 2;

            // Wait until the cell is free to write too.
            // wait_for_sequence uses std::memory_order_acquire under the hood, which prevents cell.set_value(...)
            // from being hoisted/reordered before this load.
            wait_for_sequence(cell, sequence);

            cell.set_value(std::forward<Args>(args)...);

            // publish the value. std::memory_order_release prevents cell.set_value(...) from being reordered below this store.
            cell.store_sequence(sequence + 1, std::memory_order_release);
        }

        /**
         * @brief In-place construct and tries to push the item into the queue.
         * @tparam Args Constructor argument types for \c T.
         * @param args  Arguments forwarded to \c T's constructor.
         * @return \c true if the item was enqueued, otherwise \c false.
         */
        template<class... Args>
        requires std::constructible_from<T, Args...> && std::is_nothrow_constructible_v<T, Args...>
        [[nodiscard]] bool try_emplace(Args&&... args) noexcept {
            auto& head = m_ticket_dispenser.head();
            auto expected = head.load(std::memory_order_relaxed);

            while (true) {
                const auto [index, cycle] = m_ticket_dispenser.compute_ticket(expected);
                auto& cell = m_buffer[index];
                const auto sequence = cycle * 2;

                if (sequence == cell.load_sequence(std::memory_order_acquire)) {
                    if (head.compare_exchange_strong(expected, expected + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
                        cell.set_value(std::forward<Args>(args)...);
                        cell.store_sequence(sequence + 1, std::memory_order_release);
                        return true;
                    }
                }
                else {
                    const auto prev = std::exchange(expected, head.load(std::memory_order_relaxed));
                    if (expected == prev) {
                        return false;
                    }
                }
            }
        }

        /**
        * @brief Pop and return the next item from the queue.
        * @return The next available item.
        * @note Blocks until an item becomes available.
        */
        [[nodiscard]] T pop() noexcept {
            const auto [index, cycle] = m_ticket_dispenser.next_consumer();

            auto& cell = m_buffer[index];
            const auto sequence = cycle * 2 + 1;

            // Wait until the slot has been filled.
            // wait_for_sequence uses std::memory_order_acquire under the hood, which establishes
            // a happens-before relationship with the producer's release (publish → consume)
            wait_for_sequence(cell, sequence);

            // safe to read payload after the acquire above
            auto val = cell.get_value();

            // mark the slot as free. std::memory_order_release prevents cell.get_value() from being reordered below this store.
            cell.store_sequence(sequence + 1, std::memory_order_release);
            return val;
        }

        /**
        * @brief Pop and write the next item from the queue to the provided reference.
        * @return @param out Reference to receive the next available item.
        * @note Blocks until an item becomes available.
        */
        void pop(T& out) noexcept {
            const auto [index, cycle] = m_ticket_dispenser.next_consumer();

            auto& cell = m_buffer[index];
            const auto sequence = cycle * 2 + 1;

            // Wait until the slot has been filled.
            // wait_for_sequence uses std::memory_order_acquire under the hood, which establishes
            // a happens-before relationship with the producer's release (publish → consume)
            wait_for_sequence(cell, sequence);

            // safe to read payload after the acquire above
            out = cell.get_value();

            // mark the slot as free. std::memory_order_release prevents cell.get_value() from being reordered below this store.
            cell.store_sequence(sequence + 1, std::memory_order_release);
        }

        /**
        * @brief Try to pop an item from the queue without blocking.
        * @return The item if available; otherwise \c std::nullopt.
        */
        [[nodiscard]] std::optional<T> try_pop() noexcept {
            auto& tail = m_ticket_dispenser.tail();
            auto expected = tail.load(std::memory_order_relaxed);

            while (true) {
                const auto [index, cycle] = m_ticket_dispenser.compute_ticket(expected);
                auto& cell = m_buffer[index];
                const auto sequence = cycle * 2 + 1;

                if (sequence == cell.load_sequence(std::memory_order_acquire)) {
                    if (tail.compare_exchange_strong(expected, expected + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
                        auto val = cell.get_value();
                        cell.store_sequence(sequence + 1, std::memory_order_release);
                        return val;
                    }
                }
                else {
                    const auto prev = std::exchange(expected, tail.load(std::memory_order_relaxed));
                    if (expected == prev) {
                        return std::nullopt;
                    }
                }
            }
        }

        /**
        * @brief Try to pop an item from the queue without blocking.
        * @param out Reference to receive the item if available
        * @return true if an item was available and written into the output reference, otherwise false.
        * @note the item reference is only written too if the function returns true.
        */
        [[nodiscard]] bool try_pop(T& out) noexcept {
            auto& tail = m_ticket_dispenser.tail();
            auto expected = tail.load(std::memory_order_relaxed);

            while (true) {
                const auto [index, cycle] = m_ticket_dispenser.compute_ticket(expected);
                auto& cell = m_buffer[index];
                const auto sequence = cycle * 2 + 1;

                if (sequence == cell.load_sequence(std::memory_order_acquire)) {
                    if (tail.compare_exchange_strong(expected, expected + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
                        out = cell.get_value();
                        cell.store_sequence(sequence + 1, std::memory_order_release);
                        return true;
                    }
                }
                else {
                    const auto prev = std::exchange(expected, tail.load(std::memory_order_relaxed));
                    if (expected == prev) {
                        return false;
                    }
                }
            }
        }

        /**
        * @brief Returns current size (approximate, due to concurrency).
        *
        * @return The number of elements logically in the deque
        */
        [[nodiscard]] std::size_t size() const noexcept { return static_cast<ptrdiff_t>(m_ticket_dispenser.load_head() - m_ticket_dispenser.load_tail()); }

        /**
        * @brief Checks if the queue is empty (approximate, due to concurrency).
        * @return true if the queue appears empty; false otherwise.
        */
        [[nodiscard]] bool empty() const noexcept { return size() <= 0; }

        /**
        * @brief Returns the current capacity of the queue.
        *
        * @return The capacity of the queue.
        */
        [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }
    private:
        static inline void wait_for_sequence(cell<T>& cell, std::size_t expected) noexcept {
            // Wait until the cell's sequence equals `expected`.
            // std::memory_order_acquire order is needed here to prevent subsequent
            // reads/writes being reordered above the load.
            backoff bo;
            while(expected != cell.load_sequence(std::memory_order_acquire)) {
                if constexpr (WaitMode == wait_mode::busy_wait) {
                    hint::spin_loop();
                }
                else {
                    bo.spin();
                }
            }
        }

        std::size_t m_capacity;
        [[no_unique_address]] Allocator m_allocator;

        cell<T>* m_buffer;
        ticker_dispenser m_ticket_dispenser;
    };
}