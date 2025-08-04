#pragma once
#include <array>
#include <atomic>
#include <limits>

namespace thunder {
    /**
     * @brief A fixed-size, lock-free concurrent bitset.
     *
     * This class provides atomic access to individual bits in a fixed-size bitset.
     * All operations are performed using atomic memory operations, allowing for
     * thread-safe concurrent reads and writes to bits without locks.
     *
     * All member functions are marked `noexcept` and do not throw exceptions.
     *
     * Memory ordering is controlled by the caller for each operation, enabling fine-grained
     * control over synchronization relationships.
     *
     * @tparam Size The number of bits in the bitset.
     *
     * @note
     * There is intentionally no default memory ordering for operations like `set`, `reset`, or `test`.
     * Callers must explicitly specify the appropriate `std::memory_order` for each operation.
     * This design avoids accidental reliance on `memory_order_seq_cst`, which can lead to unclear
     * semantics and unnecessary synchronization costs.
     * Unlike C++ atomics, which default to `seq_cst`, we require an explicit choice to force the user
     * to consider their concurrency model. This prevents "accidental" correctness and clarifies intent.
     */
    template<size_t Size>
    class concurrent_bitset {
    public:
        concurrent_bitset() noexcept : m_data() {}

        concurrent_bitset(const concurrent_bitset&) = delete;
        concurrent_bitset& operator=(const concurrent_bitset&) = delete;

        /**
        * @brief Sets the bit at the given index to 1.
        *
        * @param index The bit index to set.
        * @param order Memory order to use for the atomic operation.
        * @return True if the bit was previously set; false otherwise.
        *
        * @note Passing an index >= Size results in undefined behavior.
        */
        bool set(const size_t index, std::memory_order order) noexcept {
            const auto mask = make_mask(index);
            return m_data[block_index(index)].fetch_or(mask, order) & mask;
        }

        /**
         * @brief Sets or clears the bit at the given index.
         *
         * @param index The bit index to modify.
         * @param value True to set the bit, false to clear it.
         * @param order Memory order to use for the atomic operation.
         * @return True if the bit was previously set; false otherwise.
         *
         * @note Passing an index >= Size results in undefined behavior.
         */
        bool set(const size_t index, const bool value, const std::memory_order order) noexcept {
            return value ? set(index, order) : reset(index, order);
        }

        /**
         * @brief Clears the bit at the given index (sets it to 0).
         *
         * @param index The bit index to clear.
         * @param order Memory order to use for the atomic operation.
         * @return True if the bit was previously set; false otherwise.
         *
         * @note Passing an index >= Size results in undefined behavior.
         */
        bool reset(const size_t index, std::memory_order order) noexcept {
            const auto mask = make_mask(index);
            return m_data[block_index(index)].fetch_and(~mask, order) & mask;
        }

        /**
        * @brief Checks if the bit at the given index is set.
        *
        * @param index The bit index to check.
        * @param order Memory order to use for the atomic load.
        * @return True if the bit is set; false otherwise.
        *
        * @note Passing an index >= Size results in undefined behavior.
        */
        [[nodiscard]] bool test(const size_t index, std::memory_order order) const noexcept {
            const auto mask = make_mask(index);
            return m_data[block_index(index)].load(order) & mask;
        }

        /**
        * @brief Returns whether the bit at the given index is set (same as test).
        *
        * @note Passing an index >= Size results in undefined behavior.
        */
        [[nodiscard]] bool get(const size_t index, std::memory_order order) const noexcept {
            return test(index, order);
        }

        /**
         * @brief Sequentially consistent read of a bit.
         *
         * @return True if the bit is set; false otherwise.
         *
         * @note Passing an index >= Size results in undefined behavior.
         */
        bool operator[](const size_t index) const noexcept {
            return test(index, std::memory_order_seq_cst);
        }

        /**
         * @brief Returns the total number of bits in the bitset.
         */
        static constexpr auto size() noexcept { return Size; }
    private:
        using lockFreeBlockType = std::atomic_unsigned_lock_free;

        // Returns the index of the atomic block containing the given bit.
        static constexpr size_t block_index(const size_t bit) noexcept {
            return bit / m_bitsPerBlock;
        }

        // Returns the offset of the bit within its atomic block.
        static constexpr size_t bit_offset(const size_t bit) noexcept {
            return bit % m_bitsPerBlock;
        }

        static constexpr lockFreeBlockType::value_type make_mask(const size_t offset) noexcept {
            return static_cast<lockFreeBlockType::value_type>(1) << offset;
        }

        static constexpr size_t m_bitsPerBlock = std::numeric_limits<lockFreeBlockType::value_type>::digits;
        static constexpr size_t m_numBlocks = (Size + m_bitsPerBlock - 1) / m_bitsPerBlock;

        std::array<lockFreeBlockType, m_numBlocks> m_data;
    };
}
