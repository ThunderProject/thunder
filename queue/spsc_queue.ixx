module;

#include <optional>
#include <vector>
#include <new>
#include <atomic>
#include <limits>
#include <array>
#include <stdexcept>
#include <libassert/assert.hpp>
export module spsc_queue;

import thread_parker;
import backoff;

namespace thunder::spsc {
    template<class T, class Allocator = std::allocator<T>>
    struct buffer_storage {
        explicit buffer_storage(const std::size_t capacity, const Allocator& allocator = Allocator())
            :
            capacity(capacity + 1),
            data(allocator)
        {
            data.resize(capacity + 2 * padding);
        }

        buffer_storage(const buffer_storage &lhs) = delete;
        buffer_storage &operator=(const buffer_storage &lhs) = delete;
        buffer_storage(buffer_storage &&lhs) = delete;
        buffer_storage &operator=(buffer_storage &&lhs) = delete;

        ~buffer_storage() = default;

        static constexpr std::size_t padding = (std::hardware_destructive_interference_size - 1) / sizeof(T) + 1;
        static constexpr std::size_t max_size = std::numeric_limits<std::size_t>::max();

        const std::size_t capacity;
        std::vector<T, Allocator> data;
    };

    export enum class wait_mode {
        busy_wait, // Pure spin-waiting, lowest latency but can waste CPU cycles.
        backoff_spin, // spinning with exponential backoff to reduce contention.
        backoff_snooze, //firsts spin with backoff, then park/unpark threads if needed
    };

    template<wait_mode mode>
    struct wait_policy;

    template<>
    struct wait_policy<wait_mode::busy_wait> {
        static void reset(backoff&) noexcept {}
        static void idle(backoff&, thread_parker*) noexcept {}
        static void notify(thread_parker*) noexcept {}
    };

    template<>
    struct wait_policy<wait_mode::backoff_spin> {
        static void reset(backoff& backoff) noexcept {
            backoff.reset();
        }

        static void idle(backoff& backoff, thread_parker*) noexcept {
            backoff.spin();
        }

        static void notify(thread_parker*) noexcept {}
    };

    template<>
    struct wait_policy<wait_mode::backoff_snooze> {
        static void reset(backoff& backoff) noexcept {
            backoff.reset();
        }

        static void idle(backoff& backoff, thread_parker* parker) noexcept {
            DEBUG_ASSERT(parker != nullptr, "Parker must not be null in backoff_snooze mode");
            [[assume(parker != nullptr)]];
            backoff.is_completed() && parker
                ? parker->park()
                : backoff.snooze();
        }

        static void notify(thread_parker* parker) noexcept {
            DEBUG_ASSERT(parker != nullptr, "Parker must not be null in backoff_snooze mode");
            [[assume(parker != nullptr)]];
            if (parker) [[likely]] {
                parker->unpark();
            }
        }
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
    class wait_storage<wait_mode::backoff_spin> {
    public:
        thread_parker* consumer_parker() noexcept { return std::addressof(m_consumer); }
        thread_parker* producer_parker() noexcept { return  std::addressof(m_producer); }
        void notify_consumer() noexcept { m_consumer.unpark(); }
        void notify_producer() noexcept { m_producer.unpark(); }
    private:
        thread_parker m_consumer{};
        thread_parker m_producer{};
    };

    template<>
    class wait_storage<wait_mode::backoff_snooze> {
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
     * and one consumer thread (calling \c pop / \c try_pop).
     * @tparam T         Item type.
     * @tparam WaitMode  The WaitMode to use.
     * @tparam Allocator Allocator type for storage.
     */
    export template<
        class T,
        wait_mode WaitMode = wait_mode::busy_wait,
        class Allocator = std::allocator<T>
    >
    class queue : buffer_storage<T, Allocator>, wait_storage<WaitMode> { // We inherit wait_storage so that we can take advantage of EBO when WaitMode is wait_mode::busy_wait
    public:
        using buffer_storage = buffer_storage<T, Allocator>;

        /**
         * @brief Construct a queue with a given logical capacity.
         * @param capacity   Maximum number of elements that can be stored concurrently.
         * @param allocator  Allocator instance for the underlying storage.
         */
        explicit queue(const std::size_t capacity, const Allocator& allocator = Allocator())
            :
            buffer_storage(capacity, allocator),
            m_inner(this)
        {
            m_reader.capacity = buffer_storage::capacity;
        }

        queue(const queue &lhs) = delete;
        queue &operator=(const queue &lhs) = delete;
        queue(queue &&lhs) = delete;
        queue &operator=(queue &&lhs) = delete;
        ~queue() = default;

        /**
         * @brief Push an item into the queue.
         * @param item The item to copy into the queue.
         * @note Blocks if the queue is full until space is available.
         */
        void push(const T& item) noexcept(std::is_nothrow_copy_assignable_v<T>) { m_inner.push(item); }


        /**
         * @brief Push an item into the queue.
         * @param item The item to move into the queue.
         * @note Blocks if the queue is full until space is available.
         */
        void push(T&& item) noexcept(std::is_nothrow_move_assignable_v<T>) { m_inner.push(std::move(item)); }

        /**
        * @brief Tries to push an item into the queue without blocking.
        * @param item The item to copy into the queue.
        * @return \c true if the item was enqueued, otherwise \c false.
        */
        [[nodiscard]] bool try_push(const T& item) noexcept(std::is_nothrow_copy_assignable_v<T>) { return m_inner.try_push(item); }

        /**
       * @brief Tries to push an item into the queue without blocking.
       * @param item The item to move into the queue.
       * @return \c true if the item was enqueued, otherwise \c false.
       */
        [[nodiscard]] bool try_push(T&& item) noexcept(std::is_nothrow_move_assignable_v<T>) { return m_inner.try_push(std::move(item)); }

        /**
         * @brief In-place construct and push the item into the queue.
         * @tparam Args Constructor argument types for \c T.
         * @param args  Arguments forwarded to \c T's constructor.
         * @note Blocks if the queue is full until space is available.
         */
        template<class... Args>
        void emplace(Args&&... args) noexcept(std::conjunction_v<std::is_nothrow_constructible<T, Args...>, std::is_nothrow_move_constructible<T>>)
        requires std::constructible_from<T, Args...> {
            m_inner.emplace(std::forward<Args>(args)...);
        }

        /**
         * @brief In-place construct and tries to push the item into the queue.
         * @tparam Args Constructor argument types for \c T.
         * @param args  Arguments forwarded to \c T's constructor.
         * @return \c true if the item was enqueued, otherwise \c false.
         */
        template<class... Args>
        void try_emplace(Args&&... args) noexcept(std::conjunction_v<std::is_nothrow_constructible<T, Args...>, std::is_nothrow_move_constructible<T>>)
        requires std::constructible_from<T, Args...> {
            m_inner.try_emplace(std::forward<Args>(args)...);
        }

        /**
        * @brief Pop and return the next item from the queue.
        * @return The next available item.
        * @note Blocks until an item becomes available.
        */
        [[nodiscard]] T pop() noexcept(std::is_nothrow_move_assignable_v<T>) { return m_inner.pop(); }

        /**
        * @brief Pop and write the next item from the queue to the provided reference.
        * @return @param item Reference to receive the next available item.
        * @note Blocks until an item becomes available.
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
        * @return true if an item was available and written into the output reference, otherwise false.
        * @note the item reference is only written too if the function returns true.
        */
        [[nodiscard]] bool try_pop(T& item) noexcept(std::is_nothrow_copy_assignable_v<T>) { return m_inner.try_pop(item); }

        /**
        * @brief Returns current size (approximate, due to concurrency).
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
            return buffer_storage::capacity - readIndex + writeIndex;
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
        [[nodiscard]] std::size_t capacity() const noexcept { return buffer_storage::capacity -1; }
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
            * @brief Pushes an item into the queue.
            *
            * Spins if the queue is full
            *
            * @tparam U   Any type that can be forwarded into a \c T \c
            * @param item item to push.
            */
            template<class U>
            void push(U&& item) {
                //a relaxed load is fine here because the producer is the only writer of writeIndex
                const auto writeIndex = m_queue->m_writer.writeIndex.load(std::memory_order_relaxed);
                const auto nextWriteIndex = writeIndex == m_queue->buffer_storage::capacity - 1 ? 0 : writeIndex + 1;

                // If the buffer is full, refresh our cached readIndex by loading the consumer's readIndex.
                // We need acquire ordering here so that, once we observe the consumer’s release-store to readIndex,
                // we also know the consumer is done reading the slot we might overwrite later.
                if constexpr (WaitMode == wait_mode::busy_wait) {
                    while (nextWriteIndex == m_queue->m_writer.cachedReadIndex) {
                        m_queue->m_writer.cachedReadIndex = m_queue->m_reader.readIndex.load(std::memory_order_acquire);
                    }
                }
                else {
                    backoff backoff;
                    while (nextWriteIndex == m_queue->m_writer.cachedReadIndex) {
                        m_queue->m_writer.cachedReadIndex = m_queue->m_reader.readIndex.load(std::memory_order_acquire);

                        if (nextWriteIndex != m_queue->m_writer.cachedReadIndex) {
                            wait_policy<WaitMode>::reset(backoff);
                            break;
                        }
                        wait_policy<WaitMode>::idle(backoff, m_queue->producer_parker());
                    }
                }

                // write the payload before publishing writeIndex.
                m_queue->data()[writeIndex + m_queue->buffer_storage::padding] = std::forward<U>(item);

                // Publish the new writeIndex to the consumer.
                m_queue->m_writer.writeIndex.store(nextWriteIndex, std::memory_order_release);

                // unpark the consumer thread if it is currently parked.
                // this is a noop if WaitMode is wait_mode::busy_wait or wait_mode::backoff_spin
                m_queue->notify_consumer();
            }

            /**
            * @brief Tries to push an item into the queue.
            *
            * Returns immediately if the queue is full.
            *
            * @tparam U   Any type that can be forwarded into a \c T \c
            * @param item item to push.
            * @return \c true on success, otherwise \c false.
            */
            template<class U>
            bool try_push(U&& item) {
                const auto cap = m_queue->m_reader.capacity;

                //a relaxed load is fine here because the producer is the only writer of writeIndex
                const auto writeIndex = m_queue->m_writer.writeIndex.load(std::memory_order_relaxed);
                const auto nextWriteIndex = (writeIndex == m_queue->buffer_storage::capacity - 1) ? 0 : writeIndex + 1;

                if (nextWriteIndex == m_queue->m_writer.cachedReadIndex) {
                    // If the buffer is full, refresh our cached readIndex once by loading the consumer's readIndex.
                    // We need acquire ordering here so that, once we observe the consumer’s release-store to readIndex,
                    // we also know the consumer is done reading the slot we might overwrite later.
                    m_queue->m_writer.cachedReadIndex = m_queue->m_reader.readIndex.load(std::memory_order_acquire);

                    // buffer is still full after refresh, return false to indicate that we failed to push the item
                    if (nextWriteIndex == m_queue->m_writer.cachedReadIndex) {
                        return false;
                    }
                }

                // write the payload before publishing writeIndex.
                m_queue->data()[writeIndex + m_queue->buffer_storage::padding] = std::forward<U>(item);

                // Publish the new writeIndex to the consumer.
                m_queue->m_writer.writeIndex.store(nextWriteIndex, std::memory_order_release);
                return true;
            }

            /**
            * @brief In-place construct and push the item into the queue.
            * @tparam Args Constructor argument types for \c T.
            * @param args  Arguments forwarded to \c T's constructor.
            * @note Blocks if the queue is full until space is available.
            */
            template<class... Args>
            void emplace(Args&&... args)
            requires std::constructible_from<T, Args...> {
                T tmp(std::forward<Args>(args)...);
                push(std::move(tmp));
            }

            /**
            * @brief In-place construct and tries to push the item into the queue.
            * @tparam Args Constructor argument types for \c T.
            * @param args  Arguments forwarded to \c T's constructor.
            * @note Blocks if the queue is full until space is available.
            */
            template<class... Args>
            [[nodiscard]] bool try_emplace(Args&&... args)
            requires std::constructible_from<T, Args...> {
                T tmp(std::forward<Args>(args)...);
                return try_push(std::move(tmp));
            }

            /**
             * @brief Pops an item from the queue.
             *
             * Waits until an item is available, then removes it from the queue and
             * returns it. Blocks while the queue appears empty,
             *
             * @return The next available item.
             */
            [[nodiscard]] T pop() {
                // a relaxed load is fine here because the consumer is the only writer of readIndex
                const auto readIndex = m_queue->m_reader.readIndex.load(std::memory_order_relaxed);

                // If we currently think the queue is empty, refresh the writeIndex
                // by loading the producer’s writeIndex.
                // If we transition from "empty" to "not empty", acquire ordering on the load is needed to ensure we see the produced data.
                if constexpr (WaitMode == wait_mode::busy_wait) {
                    while (readIndex == m_queue->m_reader.cachedWriteIndex) {
                        m_queue->m_reader.cachedWriteIndex = m_queue->m_writer.writeIndex.load(std::memory_order_acquire);
                    }
                }
                else {
                    backoff backoff;
                    while (readIndex == m_queue->m_reader.cachedWriteIndex) {
                        m_queue->m_reader.cachedWriteIndex = m_queue->m_writer.writeIndex.load(std::memory_order_acquire);

                        if (readIndex != m_queue->m_reader.readIndex) {
                            wait_policy<WaitMode>::reset(backoff);
                            break;
                        }
                        wait_policy<WaitMode>::idle(backoff, m_queue->consumer_parker());
                    }
                }

                T item = std::move(m_queue->data()[readIndex + m_queue->buffer_storage::padding]);
                const auto nextReadIndex = (readIndex == m_queue->buffer_storage::capacity - 1) ? 0 : readIndex + 1;

                // Release our consumption to the producer.
                m_queue->m_reader.readIndex.store(nextReadIndex, std::memory_order_release);

                // unpark the producer thread if it is currently parked.
                // this is a noop if WaitMode is wait_mode::busy_wait or wait_mode::backoff_spin
                m_queue->notify_producer();
                return item;
            }

            /**
             * @brief Pops an item from the queue into an output parameter.
             *
             * Waits until an item is available, then removes it from the queue and
             * assigns it to the provided reference. Blocks while the queue appears empty.
             *
             * @param item Reference to receive the next available item.
             */
            void pop(T& item) {
                // a relaxed load is fine here because the consumer is the only writer of readIndex
                const auto readIndex = m_queue->m_reader.readIndex.load(std::memory_order_relaxed);

                // If we currently think the queue is empty, refresh the writeIndex
                // by loading the producer’s writeIndex.
                // If we transition from "empty" to "not empty", acquire ordering on the load is needed to ensure we see the produced data.
                if constexpr (WaitMode == wait_mode::busy_wait) {
                    while (readIndex == m_queue->m_reader.cachedWriteIndex) {
                        m_queue->m_reader.cachedWriteIndex = m_queue->m_writer.writeIndex.load(std::memory_order_acquire);
                    }
                }
                else {
                    backoff backoff;
                    while (readIndex == m_queue->m_reader.cachedWriteIndex) {
                        m_queue->m_reader.cachedWriteIndex = m_queue->m_writer.writeIndex.load(std::memory_order_acquire);

                        if (readIndex != m_queue->m_reader.readIndex) {
                            wait_policy<WaitMode>::reset(backoff);
                            break;
                        }
                        wait_policy<WaitMode>::idle(backoff, m_queue->consumer_parker());
                    }
                }

                item = m_queue->data()[readIndex + m_queue->buffer_storage::padding];
                const auto nextReadIndex = (readIndex == m_queue->buffer_storage::capacity - 1) ? 0 : readIndex + 1;

                // Release our consumption to the producer.
                m_queue->m_reader.readIndex.store(nextReadIndex, std::memory_order_release);

                // unpark the producer thread if it is currently parked.
                // this is a noop if WaitMode is wait_mode::busy_wait or wait_mode::backoff_spin
                m_queue->notify_producer();
            }

            /**
             * @brief Tries to pop an item from the queue.
             * @return \c std::optional containing the item if available, otherwise \c std::nullopt.
             */
            [[nodiscard]] std::optional<T> try_pop() {
                // a relaxed load is fine here because the consumer is the only writer of readIndex
                const auto readIndex = m_queue->m_reader.readIndex.load(std::memory_order_relaxed);

                if (readIndex == m_queue->m_reader.cachedWriteIndex) {
                    // If we currently think the queue is empty, refresh the writeIndex
                    // by loading the producer’s writeIndex (our cached index could be out of date).
                    // If we transition from "empty" to "not empty", acquire ordering on the load is needed to ensure we see the produced data.
                    m_queue->m_reader.cachedWriteIndex = m_queue->m_writer.writeIndex.load(std::memory_order_acquire);

                    // buffer is still empty after "refresh", return std::nullopt to indicate that the pop failed.
                    if (readIndex == m_queue->m_reader.cachedWriteIndex) {
                        return std::nullopt;
                    }
                }

                T item = std::move(m_queue->data()[readIndex + m_queue->buffer_storage::padding]);
                const auto nextReadIndex = (readIndex == m_queue->buffer_storage::capacity - 1) ? 0 : readIndex + 1;

                // Release our consumption to the producer.
                m_queue->m_reader.readIndex.store(nextReadIndex, std::memory_order_release);
                return item;
            }

            /**
             * @brief Tries to pop an item from the queue into an output parameter.
             * @param item Reference to receive the item if available.
             * @return \c true if an item was popped, otherwise \c false.
             */
            [[nodiscard]] bool try_pop(T& item) {
                // a relaxed load is fine here because the consumer is the only writer of readIndex
                const auto readIndex = m_queue->m_reader.readIndex.load(std::memory_order_relaxed);

                if (readIndex == m_queue->m_reader.cachedWriteIndex) {
                    // If we currently think the queue is empty, refresh the writeIndex
                    // by loading the producer’s writeIndex (our cached index could be out of date).
                    // If we transition from "empty" to "not empty", acquire ordering on the load is needed to ensure we see the produced data.
                    m_queue->m_reader.cachedWriteIndex = m_queue->m_writer.writeIndex.load(std::memory_order_acquire);

                    // buffer is still empty after "refresh", return std::nullopt to indicate that the pop failed.
                    if (readIndex == m_queue->m_reader.cachedWriteIndex) {
                        return false;
                    }
                }

                item =  m_queue->data()[readIndex + m_queue->buffer_storage::padding];
                const auto nextReadIndex = (readIndex == m_queue->buffer_storage::capacity - 1) ? 0 : readIndex + 1;

                // Release our consumption to the producer.
                m_queue->m_reader.readIndex.store(nextReadIndex, std::memory_order_release);
                return true;
            }
        private:
            queue* m_queue;
        };

        auto& data() noexcept { return buffer_storage::data; }

        struct alignas(std::hardware_destructive_interference_size) cacheline_writer {
            std::atomic<std::size_t> writeIndex{0};
            std::size_t cachedReadIndex{0};

            const std::size_t padding = buffer_storage::padding;
        } m_writer;

        struct alignas(std::hardware_destructive_interference_size) cacheline_reader {
            std::atomic<std::size_t> readIndex{0};
            std::size_t cachedWriteIndex{0};

            std::size_t capacity{};
        } m_reader;

        inner m_inner;

        thread_parker* consumer_parker() noexcept { return wait_storage<WaitMode>::consumer_parker(); }
        thread_parker* producer_parker() noexcept { return wait_storage<WaitMode>::producer_parker(); }
        void notify_consumer() noexcept { wait_storage<WaitMode>::notify_consumer(); }
        void notify_producer() noexcept { wait_storage<WaitMode>::notify_producer(); }
    };
}
