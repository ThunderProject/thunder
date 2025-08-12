#pragma once
#include <optional>
#include <vector>
#include <new>
#include <atomic>
#include <limits>
#include <array>
#include <stdexcept>
#include "../backoff/backoff.h"
#include "../parker/thread_parker.h"

namespace thunder::spsc {
    namespace details {
        static constexpr std::size_t maxBytesOnStack = 2'097'152; // 2 MBs

        template<class T, std::size_t Size>
        concept MaxStackSize = Size <= maxBytesOnStack / sizeof(T);

        template<class T, class Allocator = std::allocator<T>>
        struct heap_buffer {
            explicit heap_buffer(const std::size_t capacity, const Allocator& allocator = Allocator())
                :
                capacity(capacity + 1),
                data(allocator)
            {
                data.resize(capacity + 2 * padding);
            }

            heap_buffer(const heap_buffer &lhs) = delete;
            heap_buffer &operator=(const heap_buffer &lhs) = delete;
            heap_buffer(heap_buffer &&lhs) = delete;
            heap_buffer &operator=(heap_buffer &&lhs) = delete;

            ~heap_buffer() = default;

            static constexpr std::size_t padding = (std::hardware_destructive_interference_size - 1) / sizeof(T) + 1;
            static constexpr std::size_t max_size = std::numeric_limits<std::size_t>::max();

            const std::size_t capacity;
            std::vector<T, Allocator> data;
        };

        template<class T, std::size_t Size, class Allocator = std::allocator<T>>
        struct stack_buffer {
            explicit stack_buffer(const std::size_t capacity, const Allocator& allocator = Allocator()) {
                if (capacity) {
                    throw std::invalid_argument("Capacity in constructor is ignored for stack allocations");
                }
            }

            stack_buffer(const stack_buffer &lhs) = delete;
            stack_buffer &operator=(const stack_buffer &lhs) = delete;
            stack_buffer(stack_buffer &&lhs) = delete;
            stack_buffer &operator=(stack_buffer &&lhs) = delete;
            ~stack_buffer() = default;

            static constexpr std::size_t capacity{Size + 1};
            static constexpr std::size_t padding = (std::hardware_destructive_interference_size - 1) / sizeof(T) + 1;

            // (2 * padding) is for preventing cache contention between adjacent memory
            std::array<T, capacity + (2 * padding)> data;
        };
    }

    enum class wait_mode {
        busy_wait,
        backoff
    };

    template<wait_mode mode>
    struct wait_policy;

    template<>
    struct wait_policy<wait_mode::busy_wait> {
        static void reset(backoff& backoff) noexcept {
            backoff.reset();
        }

        static void idle(backoff& backoff, thread_parker* parker) noexcept {
            backoff.is_completed() && parker
                ? parker->park()
                : backoff.snooze();
        }

        static void notify(thread_parker* parker) noexcept {
            if (parker) {
                parker->unpark();
            }
        }
    };

    template<>
    struct wait_policy<wait_mode::backoff> {
        static void reset(backoff&) noexcept {}
        static void idle(backoff&, thread_parker*) noexcept {}
        static void notify(thread_parker*) noexcept {}
    };

    template<wait_mode Mode>
    class wait_storage;

    template<>
    class wait_storage<wait_mode::busy_wait> {
    public:
        thread_parker* consumer_parker() noexcept { return nullptr; }
        thread_parker* producer_parker() noexcept { return nullptr; }
        void notify_consumer() noexcept {}
        void notify_producer() noexcept {}
    };

    template<>
    class wait_storage<wait_mode::backoff> {
    public:
        thread_parker* consumer_parker() noexcept { return std::addressof(m_consumer); }
        thread_parker* producer_parker() noexcept { return  std::addressof(m_producer); }
        void notify_consumer() noexcept { m_consumer.unpark(); }
        void notify_producer() noexcept { m_producer.unpark(); }
    private:
        thread_parker m_consumer{};
        thread_parker m_producer{};
    };

    /**
     * @brief Single-producer, single-consumer lock-free queue.
     *
     * This queue supports exactly one producer thread (calling \c push / \c try_push / \c emplace)
     * and one consumer thread (calling \c pop / \c try_pop). It uses a ring buffer and minimal
     * atomic synchronization:
     *
     *  - Producer publishes data with a release store to \c writeIndex.
     *  - Consumer observes availability with an acquire load of \c writeIndex and then reads data.
     *  - Consumer signals slot reclamation with a release store to \c readIndex.
     *  - Producer observes space with an acquire load of \c readIndex when full.
     *
     * @tparam T         Item type.
     * @tparam Allocator Allocator type for storage.
     */
    template<
        class T,
        std::size_t Size = 0,
        wait_mode WaitMode = wait_mode::busy_wait,
        class Allocator = std::allocator<T>
    >
    requires (details::MaxStackSize<T, Size> || !Size)
    class queue : public std::conditional_t<Size == 0, details::heap_buffer<T, Allocator>, details::stack_buffer<T,Size>>, wait_storage<WaitMode> {
    public:
        using base = std::conditional_t<Size == 0, details::heap_buffer<T, Allocator>, details::stack_buffer<T, Size, Allocator>>;

        /**
         * @brief Construct a queue with a given logical capacity.
         * @param capacity   Maximum number of elements that can be stored concurrently.
         * @param allocator  Allocator instance for the underlying storage.
         */
        explicit queue(const std::size_t capacity, const Allocator& allocator = Allocator())
            :
            base(capacity, allocator),
            m_inner(this)
        {
            m_reader.capacity = base::capacity;
        }

        queue(const queue &lhs) = delete;
        queue &operator=(const queue &lhs) = delete;
        queue(queue &&lhs) = delete;
        queue &operator=(queue &&lhs) = delete;
        ~queue() = default;

        /**
         * @brief Push an item (copy semantics) into the queue.
         * @param item The item to copy into the queue.
         * @note Blocks (spins) if the queue is full until space is available.
         */
        void push(const T& item) noexcept(std::is_nothrow_copy_assignable_v<T>) { m_inner.push(item); }


        /**
         * @brief Push an item (move semantics) into the queue.
         * @param item The item to move into the queue.
         * @note Blocks (spins) if the queue is full until space is available.
         */
        void push(T&& item) noexcept(std::is_nothrow_move_assignable_v<T>) { m_inner.push(std::move(item)); }

        /**
        * @brief Tries to push an item into the queue without blocking.
        * @param item The item to copy into the queue.
        * @return \c true if the item was enqueued; \c false if the queue was full.
        */
        [[nodiscard]] bool try_push(const T& item) noexcept(std::is_nothrow_copy_assignable_v<T>) { return m_inner.try_push(item); }

        /**
       * @brief Tries to push an item into the queue without blocking.
       * @param item The item to move into the queue.
       * @return \c true if the item was enqueued; \c false if the queue was full.
       */
        [[nodiscard]] bool try_push(T&& item) noexcept(std::is_nothrow_move_assignable_v<T>) { return m_inner.try_push(std::move(item)); }

        /**
         * @brief In-place construct and push the item into the queue.
         * @tparam Args Constructor argument types for \c T.
         * @param args  Arguments forwarded to \c T's constructor.
         * @note Blocks (spins) if the queue is full until space is available.
         */
        template<class... Args>
        void emplace(Args&&... args) noexcept(std::conjunction_v<std::is_nothrow_constructible<T, Args...>, std::is_nothrow_move_constructible<T>>)
        requires std::constructible_from<T, Args...> {
            m_inner.emplace(std::forward<Args>(args)...);
        }

        /**
        * @brief Pop and return the next item from the queue.
        * @return The next available item.
        * @note Blocks (spins) until an item becomes available.
        */
        [[nodiscard]] T pop() noexcept(std::is_nothrow_move_assignable_v<T>) { return m_inner.pop(); }

        /**
        * @brief Pop and write the next item from the queue to the provided reference.
        * @return @param item Reference to receive the next available item.
        * @note Blocks (spins) until an item becomes available.
        */
        void pop(T& item) noexcept(std::is_nothrow_move_assignable_v<T>) { m_inner.pop(item); }

        /**
        * @brief Try to pop an item from the queue without blocking.
        * @return The item if available; otherwise \c std::nullopt.
        */
        [[nodiscard]] std::optional<T> try_pop() noexcept(std::is_nothrow_move_assignable_v<T>) { return m_inner.try_pop(); }

        /**
        * @brief Try to pop an item from the queue without blocking.
        * @param item Reference to receive the item if available
        * @return true if an was available and written into item, otherwise false.
        * @note item is only written too if the function returns true.
        */
        [[nodiscard]] bool try_pop(T& item) noexcept(std::is_nothrow_copy_assignable_v<T>) { return m_inner.try_pop(item); }

        /**
        * @brief Returns current size (approximate, due to concurrency).
        *
        * Computes the difference between the writeIndex and readIndex.
        *
        * @return The number of elements logically in the deque
        */
        [[nodiscard]] std::size_t size() const noexcept {
            // relaxed is safe here because ordering isn't needed for approximate size.
            const auto writeIndex = m_writer.writeIndex.load(std::memory_order_relaxed);
            const auto readIndex = m_reader.readIndex.load(std::memory_order_relaxed);

            if (writeIndex >= readIndex) {
                return writeIndex - readIndex;
            }
            return base::capacity - readIndex + writeIndex;
        }

        /**
        * @brief Checks if the queue is empty (approximate, due to concurrency).
        * @return true if the deque appears empty; false otherwise.
        */
        [[nodiscard]] bool empty() const noexcept {
            // relaxed is safe here because ordering isn't needed for approximate emptiness.
            return m_writer.writeIndex.load(std::memory_order_relaxed) == m_reader.readIndex.load(std::memory_order_relaxed);
        }

        /**
         * @brief Returns the current capacity of the queue.
         *
         * @return The capacity of the queue.
         */
        [[nodiscard]] std::size_t capacity() const noexcept { return base::capacity -1; }
    private:
        class inner {
        public:
            /**
            * @brief Construct the inner façade binding to the parent queue.
            * @param parent Pointer to the owning queue.
            */
            explicit inner(queue* parent) noexcept
                :
                m_queue(parent)
            {}

            /**
            * @brief Producer-side push by perfect-forwarding.
            *
            * Writes the item into the queue, then publishes the item to the producer with a release-store to \c writeIndex.
            * Spins if the queue is full; when full, refreshes the cached \c readIndex with an acquire
            * load to observe the consumer’s progress.
            *
            * @tparam U   Any type that can be forwarded into a \c T \c
            * @param item item to push.
            */
            template<class U>
            void push(U&& item) {
                //a relaxed load is fine here because the producer is the only writer of writeIndex
                const auto writeIndex = m_queue->m_writer.writeIndex.load(std::memory_order_relaxed);
                const auto nextWriteIndex = (writeIndex == m_queue->base::capacity - 1) ? 0 : writeIndex + 1;

                // If the buffer is full, refresh our cached readIndex by loading the consumer's readIndex.
                // We need acquire ordering here so that, once we observe the consumer’s release-store to readIndex,
                // we also know the consumer is done reading the slot we might overwrite later.
                if constexpr (WaitMode == wait_mode::busy_wait) {
                    while (nextWriteIndex == m_queue->m_writer.readIndex) {
                        m_queue->m_writer.readIndex = m_queue->m_reader.readIndex.load(std::memory_order_acquire);
                    }
                }
                else {
                    backoff backoff;
                    while (nextWriteIndex == m_queue->m_writer.readIndex) {
                        m_queue->m_writer.readIndex = m_queue->m_reader.readIndex.load(std::memory_order_acquire);

                        if (nextWriteIndex != m_queue->m_writer.readIndex) {
                            wait_policy<WaitMode>::reset(backoff);
                            break;
                        }
                        wait_policy<WaitMode>::idle(backoff, m_queue->producer_parker());
                    }
                }

                // write the payload before publishing writeIndex.
                m_queue->data()[writeIndex + m_queue->base::padding] = std::forward<U>(item);

                // Publish the new writeIndex to the consumer.
                // The release store pairs with the consumer's acquire load of m_writer.writeIndex.
                // This guarantees the item/payload write happens-before the consumer sees the index advance.
                m_queue->m_writer.writeIndex.store(nextWriteIndex, std::memory_order_release);

                m_queue->notify_consumer();
            }

            /**
            * @brief Producer-side nonblocking try_push by perfect-forwarding.
            *
            * Returns immediately if the queue is full after one acquire refresh of the cached
            * \c readIndex. On success, publishes with a release-store to \c writeIndex.
            *
            * @tparam U   Any type that can be forwarded into a \c T \c
            * @param item item to push.
            * @return \c true on success; \c false if full.
            */
            template<class U>
            bool try_push(U&& item) {
                const auto cap = m_queue->m_reader.capacity;

                //a relaxed load is fine here because the producer is the only writer of writeIndex
                const auto writeIndex = m_queue->m_writer.writeIndex.load(std::memory_order_relaxed);
                const auto nextWriteIndex = (writeIndex == m_queue->base::capacity - 1) ? 0 : writeIndex + 1;

                if (nextWriteIndex == m_queue->m_writer.readIndex) {
                    // If the buffer is full, refresh our cached readIndex once by loading the consumer's readIndex.
                    // We need acquire ordering here so that, once we observe the consumer’s release-store to readIndex,
                    // we also know the consumer is done reading the slot we might overwrite later.
                    m_queue->m_writer.readIndex = m_queue->m_reader.readIndex.load(std::memory_order_acquire);

                    // buffer is still full after refresh, return false to indicate that we failed to push the item
                    if (nextWriteIndex == m_queue->m_writer.readIndex) {
                        return false;
                    }
                }

                // write the payload before publishing writeIndex.
                m_queue->data()[writeIndex + m_queue->base::padding] = std::forward<U>(item);

                // Publish the new writeIndex to the consumer.
                // The release store pairs with the consumer's acquire load of m_writer.writeIndex.
                // This guarantees the item/payload write happens-before the consumer sees the index advance.
                m_queue->m_writer.writeIndex.store(nextWriteIndex, std::memory_order_release);
                return true;
            }

            /**
            * @brief Producer-side in-place construction convenience.
            * @tparam Args Constructor argument types for \c T.
            * @param args  Arguments forwarded to construct a temporary \c T which is then pushed.
            */
            template<class... Args>
            void emplace(Args&&... args)
            requires std::constructible_from<T, Args...> {
                T tmp(std::forward<Args>(args)...);
                push(std::move(tmp));
            }

            /**
             * @brief Consumer-side blocking pop.
             *
             * Waits until an item is available, then removes it from the queue and
             * returns it. Blocks (spins) while the queue appears empty,
             * refreshing the cached write index until new data arrives.
             *
             * @return The next available item, moved from the queue.
             */
            [[nodiscard]] T pop() {
                // a relaxed load is fine here because the consumer is the only writer of readIndex
                const auto readIndex = m_queue->m_reader.readIndex.load(std::memory_order_relaxed);

                // If we currently think the queue is empty (based on our local cached writeIndex), refresh the writeIndex
                // by loading the producer’s writeIndex.
                // If we transition from "empty" to "not empty", acquire ordering on the load is needed to ensure we see the produced data.
                if constexpr (WaitMode == wait_mode::busy_wait) {
                    while (readIndex == m_queue->m_reader.writeIndex) {
                        m_queue->m_reader.writeIndex = m_queue->m_writer.writeIndex.load(std::memory_order_acquire);
                    }
                }
                else {
                    backoff backoff;
                    while (readIndex == m_queue->m_reader.writeIndex) {
                        m_queue->m_reader.writeIndex = m_queue->m_writer.writeIndex.load(std::memory_order_acquire);

                        if (readIndex != m_queue->m_reader.readIndex) {
                            wait_policy<WaitMode>::reset(backoff);
                            break;
                        }
                        wait_policy<WaitMode>::idle(backoff, m_queue->consumer_parker());
                    }
                }

                T item = std::move(m_queue->data()[readIndex + m_queue->base::padding]);
                const auto nextReadIndex = (readIndex == m_queue->base::capacity - 1) ? 0 : readIndex + 1;

                // Release our consumption to the producer (who acquires readIndex when checking for space).
                // This prevents the producer from overwriting the slot before our read happens-before.
                m_queue->m_reader.readIndex.store(nextReadIndex, std::memory_order_release);

                m_queue->notify_producer();
                return item;
            }

            /**
             * @brief Consumer-side blocking pop into an output parameter.
             *
             * Waits until an item is available, then removes it from the queue and
             * assigns it to the provided reference. Blocks
             * (spins) while the queue appears empty, refreshing the cached write
             * index until new data arrives.
             *
             * @param item Reference to receive the next available item.
             */
            void pop(T& item) {
                // a relaxed load is fine here because the consumer is the only writer of readIndex
                const auto readIndex = m_queue->m_reader.readIndex.load(std::memory_order_relaxed);

                // If we currently think the queue is empty (based on our local cached writeIndex), refresh the writeIndex
                // by loading the producer’s writeIndex.
                // If we transition from "empty" to "not empty", acquire ordering on the load is needed to ensure we see the produced data.
                if constexpr (WaitMode == wait_mode::busy_wait) {
                    while (readIndex == m_queue->m_reader.writeIndex) {
                        m_queue->m_reader.writeIndex = m_queue->m_writer.writeIndex.load(std::memory_order_acquire);
                    }
                }
                else {
                    backoff backoff;
                    while (readIndex == m_queue->m_reader.writeIndex) {
                        m_queue->m_reader.writeIndex = m_queue->m_writer.writeIndex.load(std::memory_order_acquire);

                        if (readIndex != m_queue->m_reader.readIndex) {
                            wait_policy<WaitMode>::reset(backoff);
                            break;
                        }
                        wait_policy<WaitMode>::idle(backoff, m_queue->consumer_parker());
                    }
                }

                item = m_queue->data()[readIndex + m_queue->base::padding];
                const auto nextReadIndex = (readIndex == m_queue->base::capacity - 1) ? 0 : readIndex + 1;

                // Release our consumption to the producer (who acquires readIndex when checking for space).
                // This prevents the producer from overwriting the slot before our read happens-before.
                m_queue->m_reader.readIndex.store(nextReadIndex, std::memory_order_release);

                m_queue->notify_producer();
            }

            /**
             * @brief Consumer-side nonblocking try_pop.
             *
             * Returns immediately if the queue appears empty after one refresh
             * of the cached write index. On success, moves the item from the queue
             * into a newly constructed std::optional<T> and returns it.
             *
             * @return std::optional containing the item if available, otherwise empty.
             */
            [[nodiscard]] std::optional<T> try_pop() {
                // a relaxed load is fine here because the consumer is the only writer of readIndex
                const auto readIndex = m_queue->m_reader.readIndex.load(std::memory_order_relaxed);

                if (readIndex == m_queue->m_reader.writeIndex) {
                    // If we currently think the queue is empty (based on our local cached writeIndex), refresh the writeIndex
                    // by loading the producer’s writeIndex (our cached index could be out of date).
                    // If we transition from "empty" to "not empty", acquire ordering on the load is needed to ensure we see the produced data.
                    m_queue->m_reader.writeIndex = m_queue->m_writer.writeIndex.load(std::memory_order_acquire);

                    // buffer is still empty after "refresh", return std::nullopt to indicate that the pop failed.
                    if (readIndex == m_queue->m_reader.writeIndex) {
                        return std::nullopt;
                    }
                }

                T item = std::move(m_queue->data()[readIndex + m_queue->base::padding]);
                const auto nextReadIndex = (readIndex == m_queue->base::capacity - 1) ? 0 : readIndex + 1;

                // Release our consumption to the producer (who acquires readIndex when checking for space).
                // This prevents the producer from overwriting the slot before our read happens-before.
                m_queue->m_reader.readIndex.store(nextReadIndex, std::memory_order_release);
                return item;
            }

            /**
             * @brief Consumer-side nonblocking try_pop into an output parameter.
             *
             * Attempts to remove the next item from the queue and assign it to the
             * provided reference. If the queue is empty after
             * one refresh of the cached write index, leaves item unchanged and
             * returns false.
             *
             * @param item Reference to receive the item if available.
             * @return true if an item was dequeued, false if the queue was empty.
             */
            [[nodiscard]] bool try_pop(T& item) {
                // a relaxed load is fine here because the consumer is the only writer of readIndex
                const auto readIndex = m_queue->m_reader.readIndex.load(std::memory_order_relaxed);

                if (readIndex == m_queue->m_reader.writeIndex) {
                    // If we currently think the queue is empty (based on our local cached writeIndex), refresh the writeIndex
                    // by loading the producer’s writeIndex (our cached index could be out of date).
                    // If we transition from "empty" to "not empty", acquire ordering on the load is needed to ensure we see the produced data.
                    m_queue->m_reader.writeIndex = m_queue->m_writer.writeIndex.load(std::memory_order_acquire);

                    // buffer is still empty after "refresh", return std::nullopt to indicate that the pop failed.
                    if (readIndex == m_queue->m_reader.writeIndex) {
                        return false;
                    }
                }

                item =  m_queue->data()[readIndex + m_queue->base::padding];
                const auto nextReadIndex = (readIndex == m_queue->base::capacity - 1) ? 0 : readIndex + 1;

                // Release our consumption to the producer (who acquires readIndex when checking for space).
                // This prevents the producer from overwriting the slot before our read happens-before.
                m_queue->m_reader.readIndex.store(nextReadIndex, std::memory_order_release);
                return true;
            }
        private:
            queue* m_queue;
        };

        auto& data() noexcept { return base::data; }

        struct alignas(std::hardware_destructive_interference_size) cacheline_writer {
            std::atomic<std::size_t> writeIndex{0};
            std::size_t readIndex{0};

            const std::size_t padding = base::padding;
        } m_writer;

        struct alignas(std::hardware_destructive_interference_size) cacheline_reader {
            std::atomic<std::size_t> readIndex{0};
            std::size_t writeIndex{0};

            std::size_t capacity{};
        } m_reader;

        inner m_inner;

        thread_parker* consumer_parker() noexcept { return wait_storage<WaitMode>::consumer_parker(); }
        thread_parker* producer_parker() noexcept { return wait_storage<WaitMode>::producer_parker(); }
        void notify_consumer() noexcept { wait_storage<WaitMode>::notify_consumer(); }
        void notify_producer() noexcept { wait_storage<WaitMode>::notify_producer(); }
    };
}
