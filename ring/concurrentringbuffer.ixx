module;
#include <atomic>
#include <bit>
#include <expected>
#include <format>
#include <memory>
#include <ranges>
#include <string>

export module concurrent_ringbuffer;

namespace thunder {
    /**
    * @brief A lock-free, fixed-capacity ring buffer for concurrent data access.
    *
    * This class provides a circular buffer that supports atomic, index-based access
    * to elements using `std::atomic<T>`. It offers low-level building blocks for
    * concurrent data structures, but does not implement coordination or synchronization.
    *
    * The buffer assumes power-of-two capacity and uses index wrapping to stay within bounds.
    * All element accesses use relaxed memory ordering, which supports atomic operations
    * but does not enforce ordering or synchronization between threads.
    *
    * This class does not throw exceptions. Allocation failure during
    * resizing is reported via a std::expected return value.
    *
    * @note This class is a low-level storage primitive and does not:
    * - Coordinate access between producers and consumers
    * - Enforce memory ordering (e.g., acquire/release semantics)
    * - Prevent data overwrites (e.g., multiple writers to the same index)
    * - Track ownership or validity of indices
    *
    * @tparam T The element type, which must be trivially copyable and compatible with std::atomic.
    */
    export template<class T>
    class concurrentringbuffer {
        static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
    public:
        /**
         * @brief Constructs a ring buffer with a power-of-two capacity.
         *
         * Rounds the requested capacity up to the nearest power of two to ensure
         * correct and efficient index wrapping. Allocates internal storage for the
         * resulting capacity using uninitialized atomic slots.
         *
         * @param requested_capacity The minimum number of elements the buffer should be able to hold.
         */
        constexpr explicit concurrentringbuffer(const uint64_t requested_capacity)
            :
            m_capacity(std::bit_ceil(requested_capacity)),
            m_capacity_mask(m_capacity - 1),
            m_buffer(std::make_unique_for_overwrite<atomic_array>(m_capacity))
        {}

        [[nodiscard]] constexpr auto capacity() const noexcept { return m_capacity; }

        /**
         * @brief Atomically stores a value at the specified position in the ring buffer.
         *
         * This function writes the given value to the buffer slot corresponding to the
         * provided index, wrapping the index as needed to stay within the buffer bounds.
         *
         * @param index The logical index at which to store the value.
         * @param value The value to store in the buffer.
         *
         * @note Uses std::memory_order_relaxed: ensures atomicity, but not synchronization
         * or ordering with respect to other operations.
         */
        constexpr void write_at(const int64_t index, const T& value) noexcept {
            // Performs an atomic store with relaxed memory ordering.
            // Guarantees atomicity, but does not enforce ordering
            // relative to other operations in this or other threads.
            (m_buffer.get() + (index & m_capacity_mask))->store(value, std::memory_order_relaxed);
        }

        /**
         * @brief Atomically retrieves a value from the specified position in the ring buffer.
         *
         * This function reads the value stored at the buffer slot corresponding to the
         * provided index, wrapping the index as needed to stay within the buffer bounds.
         *
         * @param index The logical index from which to read the value.
         * @return The value stored in the buffer at the given position.
         *
         * @note Uses std::memory_order_relaxed: ensures atomicity, but not synchronization
         *       or ordering with respect to other operations.
         */
        [[nodiscard]] auto read_at(const int64_t index) const noexcept {
            // Performs an atomic load with relaxed memory ordering.
            // Guarantees atomicity, but does not enforce ordering
            // relative to other operations in this or other threads.
            return m_buffer[index & m_capacity_mask].load(std::memory_order_relaxed);
        }

        /**
         * @brief Provides read-only access to an element at the specified index.
         *
         * Returns the value at the logical index in the buffer, wrapping the index
         * to stay within bounds. This operation performs an atomic load using relaxed
         * memory ordering, ensuring atomicity but not synchronization with other threads.
         *
         * @param index The logical index of the element to retrieve.
         * @return The value stored at the corresponding position in the buffer.
         *
         * @note Uses std::memory_order_relaxed: ensures atomic access, but does not
         *       provide ordering guarantees relative to other operations.
         */
        [[nodiscard]] auto operator[](const int64_t index) const noexcept {
            return read_at(index);
        }

        /**
         * @brief Creates a resized copy of the ring buffer with increased capacity.
         *
         * Allocates a new ring buffer with the next power-of-two capacity large enough
         * to store at least one additional element. Copies all currently stored elements
         * from the range [top, bottom) — where `top` is the start of valid data and
         * `bottom` is one past the last valid element.
         *
         * @param bottom The logical end index (exclusive) of the valid data.
         * @param top The logical start index (inclusive) of the valid data.
         * @return std::expected containing the newly allocated buffer, or an error message
         *         if memory allocation fails.
         *
         * @note The returned pointer must be manually deleted. Discarding the return
         *       value results in a memory leak.
         */
        [[nodiscard("Discarding leaks memory!")]]
        constexpr std::expected<concurrentringbuffer*, std::string> resize(int64_t bottom, int64_t top) const noexcept {
            try {
                auto resizedBuffer = new concurrentringbuffer(std::bit_ceil(m_capacity + 1));

                for(auto i : std::ranges::views::iota(top, bottom)) {
                    resizedBuffer->write_at(i, read_at(i));
                }
                return resizedBuffer;
            }
            catch(const std::exception& e) {
                return std::unexpected(std::format("Failed to allocate resized buffer: {}", e.what()));
            }
        }
    private:
        using atomic_array = std::atomic<T>[];

        uint64_t m_capacity;
        uint64_t m_capacity_mask;
        std::unique_ptr<atomic_array> m_buffer;
    };
}
