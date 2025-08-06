#pragma once
#include <new>
#include <vector>
#include "ring/concurrentringbuffer.h"

namespace thunder {
    enum class PopFailureReason {
        FailedRace,
        EmptyQueue
    };

    enum class StealFailureReason {
        FailedRace,
        EmptyQueue
    };

    enum class Flavor {
        Fifo, // first-in first-out flavor
        Lifo  // last-in first-out flavor
    };

    /**
    * @brief A lock-free, concurrent, dynamically resizable worker queue.
    *
    * This is a FIFO or LIFO queue that is owned by a single thread, but other threads may steal
    * tasks from it.
    *
    * Only the owner thread is allowed to call `push()` and `pop()`. Multiple consumer threads
    * may concurrently call `steal()`
    *
    * Internally, it uses a lock-free ring buffer.
    * The buffer grows dynamically and supports concurrent access with minimal contention.
    *
    * @tparam T The type of elements stored in the deque. Must be trivially destructible.
    */
    template<class T>
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
        {
            m_garbage.reserve(64);
        }

        /**
         * @brief Copy constructor is deleted.
         *
         * concurrent_deque is non-copyable to avoid issues with shared ownership of internal state.
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
         * Based on relaxed reads; result may be stale under concurrency.
         *
         * @return true if the deque appears empty; false otherwise.
         */
        [[nodiscard]] bool empty() const noexcept {
            return size() == 0;
        }

        /**
         * @brief Pushes an item onto the bottom of the deque.
         *
         * Adds a new element to the bottom of the queue. This method must be called only by the
         * owning (producer) thread. If the current buffer is full, it attempts to resize; if that
         * fails, the operation returns false.
         *
         * @param item The item to push.
         * @return true if the item was successfully pushed; false if resizing failed.
         */
        bool push(T&& item) noexcept {
            // std::memory_order_relaxed is sufficient because this load doesn't acquire anything from
            // another thread. m_bottom is only written in pop() which cannot be concurrent with push()
            auto bottom = m_bottom.load(std::memory_order_relaxed);

            // std::memory_order_acquire is needed because m_top is written by other threads in steal,
            // so we need to observe their updates.
            auto top = m_top.load(std::memory_order_acquire);

            // std::memory_order_relaxed is sufficient because m_buffer is only replaced by the owner thread
            auto buffer = m_buffer.load(std::memory_order_relaxed);

            if (buffer->capacity() <  (bottom - top) + 1) {
                auto bigger = buffer->resize(bottom, top);
                if (!bigger.has_value()) {
                    return false;
                }

                m_garbage.emplace_back(std::exchange(buffer, bigger));

                // std::memory_order_relaxed is sufficient because only the owner thread writes m_buffer, so no synchronization needed.
                m_buffer.store(buffer, std::memory_order_relaxed);
            }

            buffer->write_at(bottom, std::move(item));

            // std::memory_order_release is needed here because we release the item we just pushed to
            // other threads which are calling steal.
            m_bottom.store(bottom + 1, std::memory_order_release);

            return true;
        }

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
        std::expected<T, PopFailureReason> pop() noexcept {
            // std::memory_order_relaxed is sufficient because m_bottom is only written by the owner thread
            const auto bottom = m_bottom.load(std::memory_order_relaxed) -1;
            const auto buffer = m_buffer.load(std::memory_order_relaxed);

            // Temporarily decrement bottom — relaxed store is sufficient, as no ordering is required yet.
            m_bottom.store(bottom, std::memory_order_relaxed);

            // Issue a full memory fence to enforce a sequentially consistent view.
            // This ensures visibility of all prior writes (e.g., by producers) before evaluating queue state.
            std::atomic_thread_fence(std::memory_order_seq_cst);

            // Load m_top with relaxed ordering; correctness is ensured by the preceding fence and a later CAS.
            auto top = m_top.load(std::memory_order_relaxed);

            if (top <= bottom) {
                // If this is the last item, we must win a race to pop it.
                if (top == bottom) {
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

            // std::memory_order_relaxed is sufficient because we're not publishing any data.
            // No concurrent writes to m_bottom is possible
            m_bottom.store(bottom + 1, std::memory_order_relaxed);
            return std::unexpected{ PopFailureReason::EmptyQueue };
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
          *
          */
        std::expected<T, StealFailureReason> steal() noexcept {
            // Load the current top index. Note: A Key component of this algorithm is that m_top is read before m_bottom here
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
            return std::unexpected{ PopFailureReason::EmptyQueue };
        }
    private:
        alignas(std::hardware_destructive_interference_size) std::atomic_ptrdiff_t m_top{0};
        alignas(std::hardware_destructive_interference_size) std::atomic_ptrdiff_t m_bottom{0};
        alignas(std::hardware_destructive_interference_size) std::atomic<concurrentringbuffer<T>*> m_buffer{};
        std::vector<std::unique_ptr<concurrentringbuffer<T>>> m_garbage;
    };
}
