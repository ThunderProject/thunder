module;
#include <expected>
#include <new>
#include <atomic>
#include <vector>

export module concurrent_deque;
import reclaimer;
import concurrent_ringbuffer;

namespace thunder {
    export enum class reclamation_technique {
        bounded, // does not resize the buffer, push will fail when the buffer is full
        deferred, // resizes the buffer and reclaims the memory when the concurrent_deque is destroyed
    };

    template<reclamation_technique Technique, class Buffer>
    struct reclaimer_selector;

    template<typename Buffer>
    struct reclaimer_selector<reclamation_technique::bounded, Buffer> {
        using type = bounded_reclaimer<Buffer>;
    };

    template<typename Buffer>
    struct reclaimer_selector<reclamation_technique::deferred, Buffer> {
        using type = deferred_reclaimer<Buffer>;
    };

    template<reclamation_technique Technique, typename Buffer>
    using reclaimer_selector_t = typename reclaimer_selector<Technique, Buffer>::type;

    export enum class PopFailureReason {
        FailedRace,
        EmptyQueue
    };

    export enum class StealFailureReason {
        FailedRace,
        EmptyQueue
    };

    export enum class Flavor {
        Fifo, // first-in first-out flavor
        Lifo  // last-in first-out flavor
    };

    /**
    * @brief A lock-free, concurrent work-stealing deque with configurable memory management.
    *
    * This is a FIFO or LIFO queue owned by a single thread, but other threads may steal
    * tasks from it.
    *
    * Only the owner thread is allowed to call `push()` and `pop()`. Multiple consumer threads
    * may concurrently call `steal()`
    *
    * The queue internally uses a lock-free ring buffer, and its resizing behavior depends on the selected
     * reclamation strategy:
     * - `reclamation_technique::bounded`: fails `push()` when full. — fastest option, but push will fail if buffer is full.
     * - `reclamation_technique::deferred`: grows dynamically and reclaims memory on destruction. — safe, but incurs copy overhead during resize.
     *
    * @tparam T The type of elements stored in the deque. Must be trivially destructible.
    * @tparam Technique The memory reclamation policy to use when the buffer is full.
    * @tparam flavor Determines whether the owner pops items in FIFO or LIFO order.
    */
    export template<
        class T,
        reclamation_technique Technique = reclamation_technique::deferred,
        Flavor flavor = Flavor::Lifo
    >
    class concurrent_deque {
        static_assert(
            std::is_trivially_destructible_v<T>,
            "T must be trivially destructible because failed steal operations may discard potentially uninitialized or torn values without invoking destructors."
        );
    public:
        /**
         * @brief Constructs a concurrent deque with the specified initial capacity.
         *
         * @param capacity Initial capacity of the ring buffer (rounded up to power of two).
         */
        explicit concurrent_deque(ptrdiff_t capacity = 1024)
            :
            m_buffer(new concurrentringbuffer<T>(capacity))
        {}

        /**
         * @brief Copy constructor is deleted.
         *
         * Concurrent_deque is non-copyable to avoid issues with shared ownership of the internal state.
         */
        concurrent_deque(const concurrent_deque& rhs) = delete;

        /**
         * @brief Copy assignment is deleted.
         *
         * concurrent_deque cannot be reassigned, as it manages thread-local and shared state.
         */
        concurrent_deque& operator=(const concurrent_deque& rhs) = delete;

        /**
         * @brief Destroys the concurrent deque and releases all buffers.
         *
         * Frees the active buffer and any previous buffers retained in the garbage list.
         * Safe to call only when no other threads are accessing the deque.
         */
        ~concurrent_deque() noexcept { delete m_buffer.load(std::memory_order_seq_cst); }

        /**
        * @brief Returns current size (approximate, due to concurrency).
        *
        * Computes the difference between the bottom and top indices.
        *
        * @return The number of elements logically in the deque
        */
        [[nodiscard]] size_t size() const noexcept {
            // relaxed is safe here because ordering isn't needed for approximate size.
            const auto bottom = m_bottom.load(std::memory_order_relaxed);
            const auto top = m_top.load(std::memory_order_relaxed);
            return std::max(bottom - top, {0});
        }

        /**
        * @brief Returns the current capacity of the underlying ring buffer.
        *
        * @return The power-of-two capacity of the active buffer.
        */
        [[nodiscard]] constexpr auto capacity() const noexcept {
            // Relaxed is safe here because we only need atomicity. Capacity is constant per buffer.
            return m_buffer.load(std::memory_order_relaxed)->capacity();
        }

        /**
        * @brief Checks if the deque is empty (approximate).
        *
        * Based on relaxed reads; a result may be stale under concurrency.
        *
        * @return true if the deque appears empty; false otherwise.
        */
        [[nodiscard]] bool empty() const noexcept {
            return size() == 0;
        }

        /**
        * @brief Pushes an item onto the bottom of the deque.
        *
        * Adds a new element to the bottom of the queue. This method must be called only by the owning (producer) thread.
        * Behavior on buffer full depends on the reclamation technique:
        * - `bounded`: fails without modifying the buffer
        * - `deferred`: attempts to resize; fails if allocation fails
        *
        * @param item The item to push.
        * @return true if the item was successfully pushed; false if resizing failed.
        */
        bool push(const T& item) noexcept {
            // std::memory_order_relaxed is enough because this load doesn't acquire anything from
            // another thread. m_bottom is only written in pop() which cannot be concurrent with push()
            auto bottom = m_bottom.load(std::memory_order_relaxed);

            // std::memory_order_acquire is needed because m_top is written by other threads in steal,
            // so we need to observe their updates.
            auto top = m_top.load(std::memory_order_acquire);

            // std::memory_order_relaxed is enough because m_buffer is only replaced by the owner thread
            auto buffer = m_buffer.load(std::memory_order_relaxed);

            if (needs_resize(buffer, bottom, top)) [[unlikely]] {
                if constexpr (Technique == reclamation_technique::bounded) {
                    // fail fast — cannot grow or overwrite
                    return false;
                }

                auto bigger = buffer->resize(bottom, top);

                // failed to resize, return false to indicate push failure
                if (!bigger.has_value()) {
                    return false;
                }

                // replace the buffer with the new resized one and collect the old buffer for deferred reclamation
                m_reclaimer.collect(std::exchange(buffer, bigger.value()));

                // std::memory_order_relaxed is enough because only the owner thread writes m_buffer, so no synchronization needed.
                m_buffer.store(buffer, std::memory_order_relaxed);
            }

            buffer->write_at(bottom, std::move(item));

            // std::memory_order_release is needed here because we release the item we just pushed to
            // other threads which are calling steal.
            m_bottom.store(bottom + 1, std::memory_order_release);

            return true;
        }

        /**
        * @brief Removes and returns an item from the deque, based on the configured flavor.
        *
        * Dispatches to either `pop_top()` or `pop_bottom()` depending on the `Flavor` template parameter.
        *
        * - In `Flavor::Lifo`, the item is removed from the bottom.
        * - In `Flavor::Fifo`, the item is removed from the top.
        *
        * This method must only be called by the owning (producer) thread.
        *
        * @return std::expected<T, PopFailureReason>
        *         - On success: the removed item.
        *         - On failure: an error reason indicating either an empty deque or a failed contention.
        */
        std::expected<T, PopFailureReason> pop() noexcept {
            if (empty()) {
                return std::unexpected{ PopFailureReason::EmptyQueue };
            }
            if constexpr (flavor == Flavor::Fifo) {
                return pop_top();
            }
            else if constexpr (flavor == Flavor::Lifo) {
                return pop_bottom();
            }
            else {
                std::unreachable();
            }
        }

        /**
        * @brief Attempts to steal an item from the top of the deque.
        *
        * This method can be called concurrently by multiple threads.
        * If the deque is not empty, it attempts to claim and return the item at the top.
        * If successful, it returns the stolen item; otherwise, it returns a failure reason.
        *
        * @return std::expected<T, StealFailureReason>
        *         - On success: the stolen item.
        *         - On failure: an error reason indicating either an empty deque or a failed race.
        */
        std::expected<T, StealFailureReason> steal() noexcept {
            // Load the current top index. Note: A Key part of this algorithm is that m_top is read before m_bottom here
            auto top = m_top.load(std::memory_order_acquire);

            // Sequentially consistent fence to prevent reordering of the loads of m_top and m_bottom.
            std::atomic_thread_fence(std::memory_order_seq_cst);

            // std::memory_order_acquire is needed because we're acquiring items published in push().
            const auto bottom = m_bottom.load(std::memory_order_acquire);

            if (top < bottom) {
                // std::memory_order_acquire ensures visibility of the item at `top`, which was written using a relaxed store followed by a release fence.
                // Having acquire-loaded both m_top and m_bottom, the release-acquire synchronization guarantees safe access to the item.
                auto item = m_buffer.load(std::memory_order_acquire)->read_at(top);


                // Attempt to claim the slot by advancing `top` via CAS.
                // Use std::memory_order_seq_cst to establish a total order at this synchronization point.
                // On failure, relaxed ordering suffices, as no synchronization is required when the CAS fails.
                if(m_top.compare_exchange_strong(top, top + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
                    return item;
                }

                // Another thread (typically the owner) claimed the item first. Discard and report failure.
                // Item may be in a partially moved-from state and so must be trivially destructible (enforced by the static_assert at the top of the class).
                return std::unexpected{ StealFailureReason::FailedRace };
            }

            // The queue is empty from the stealing thread’s perspective.
            return std::unexpected{ StealFailureReason::EmptyQueue };
        }

        std::expected<std::vector<T>, StealFailureReason> steal_batch(size_t batchSize) noexcept {
            if (batchSize == 0) {
                return {};
            }
        }

    private:
        /**
        * @brief Removes and returns an item from the bottom of the deque.
        *
        * This method must only be called by the owning (producer) thread.
        * If the deque is not empty, it removes and returns the item at the bottom.
        * If the deque appears empty or the pop operation loses a race to a concurrent
        * steal from another thread, it fails with an appropriate reason.
        *
        * @return std::expected<T, PopFailureReason>
        *         - On success: the removed item.
        *         - On failure: an error reason indicating either an empty deque or a failed contention.
        */
         std::expected<T, PopFailureReason> pop_bottom() noexcept {
            // std::memory_order_relaxed is enough because m_bottom is only written by the owner thread
            const auto bottom = m_bottom.load(std::memory_order_relaxed) -1;
            const auto buffer = m_buffer.load(std::memory_order_relaxed);

            // Temporarily decrement bottom — relaxed store is enough, as no ordering is required yet.
            m_bottom.store(bottom, std::memory_order_relaxed);

            // Issue a full memory fence to enforce a sequentially consistent view.
            // This ensures visibility of all prior writings (e.g., by producers) before evaluating the queue state.
            std::atomic_thread_fence(std::memory_order_seq_cst);

            // Load m_top with relaxed ordering; correctness is ensured by the preceding fence and a later CAS.
            auto top = m_top.load(std::memory_order_relaxed);

            // Compute the length after the bottom index was decremented
            const auto length = bottom - top;
            if (length < 0) {
                //The queue is empty. Restore bottom and report failure
                // std::memory_order_relaxed is enough because we're not publishing any data.
                m_bottom.store(bottom + 1, std::memory_order_relaxed);
                return std::unexpected{ PopFailureReason::EmptyQueue };
            }

            // We are trying to retrieve the last item in the queue. We must win a race with any stealers to pop it.
            if (length == 0) {
                // Attempt to claim the slot by advancing `top` via CAS.
                // Use std::memory_order_seq_cst to establish a total order at this synchronization point.
                // On failure, relaxed ordering suffices, as no synchronization is required when the CAS fails.
                // A failed CAS indicates that another consumer acquired the slot first.
                if(!m_top.compare_exchange_strong(top, top + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
                    // Restore bottom and report failure
                    m_bottom.store(bottom + 1, std::memory_order_relaxed);
                    return std::unexpected{ PopFailureReason::FailedRace };
                }
                // restore bottom if we won the race
                m_bottom.store(bottom + 1, std::memory_order_relaxed);
            }
            return std::move(buffer->read_at(bottom));
        }

        /**
        * @brief Removes and returns an item from the top of the deque.
        *
        * Only the owning (producer) thread may call this method.
        * It atomically increments the top index to claim the item.
        *
        * This method does not require contention handling with stealers, but may fail if the deque is empty.
        *
        * @return std::expected<T, PopFailureReason>
        *         - On success: the removed item.
        *         - On failure: an error reason indicating the deque was empty.
        */
        std::expected<T, PopFailureReason> pop_top() noexcept {
             // std::memory_order_relaxed is enough because m_bottom is only written by the owner thread
             const auto bottom = m_bottom.load(std::memory_order_relaxed);

             // Use std::memory_order_seq_cst to enforce a total order among top updates
             auto top = m_top.fetch_add(1, std::memory_order_seq_cst);


             if (bottom - (top + 1) < 0) {
                 // std::memory_order_relaxed is enough because there is no ordering w.r.t other read/writes
                 m_top.store(top, std::memory_order_relaxed);
                 return std::unexpected{ PopFailureReason::EmptyQueue };
             }
             // std::memory_order_relaxed is enough because there is no ordering w.r.t other read/writes
             return m_buffer.load(std::memory_order_relaxed)->read_at(top);
         }

        [[nodiscard]] static bool needs_resize(const concurrentringbuffer<T>* buffer, const std::atomic_ptrdiff_t& bottom, const std::atomic_ptrdiff_t& top) noexcept {
            return buffer->capacity() < (bottom - top) + 1;
        }

        using buffer_type = concurrentringbuffer<T>;
        using reclaimer = reclaimer_selector_t<Technique, buffer_type>;

        alignas(std::hardware_destructive_interference_size) std::atomic_ptrdiff_t m_top{0};
        alignas(std::hardware_destructive_interference_size) std::atomic_ptrdiff_t m_bottom{0};
        alignas(std::hardware_destructive_interference_size) std::atomic<buffer_type*> m_buffer{};
        reclaimer m_reclaimer;
    };
}
