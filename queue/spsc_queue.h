#pragma once
#include <optional>
#include <vector>
#include <new>
#include <atomic>
#include <limits>

namespace thunder::spsc {
    namespace details {
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
    }


    template<class T, class Allocator = std::allocator<T>>
    class queue : public details::heap_buffer<T, Allocator> {
    public:
        using base = details::heap_buffer<T, Allocator>;

        explicit queue(const std::size_t capacity, const Allocator& allocator = Allocator())
            :
            base(capacity, allocator),
            m_inner(this)
        {
            m_reader.capacity = base::capacity;
        }

        void push(const T& item) { m_inner.push(item); }
        void push(T&& item) { m_inner.push(std::move(item)); }

        void force_push(const T& item) { m_inner.force_push(item); }
        void force_push(T&& item) { m_inner.force_push(std::move(item)); }

        [[nodiscard]] bool try_push(const T& item) { return m_inner.try_push(item); }
        [[nodiscard]] bool try_push(T&& item) { return  m_inner.try_push(item); }

        template<class... Args>
        void emplace(Args&&... args)
        requires std::constructible_from<T, Args...> {
            m_inner.emplace(std::forward<Args>(args)...);
        }

        template<class... Args>
        void force_emplace(Args&&... args)
        requires std::constructible_from<T, Args...> {
            m_inner.force_emplace(std::forward<Args>(args)...);
        }

        [[nodiscard]] T pop() { return m_inner.pop(); }
        [[nodiscard]] std::optional<T> try_pop() { return m_inner.try_pop(); }
    private:
        class inner {
        public:
            explicit inner(queue* parent)
                :
                m_queue(parent)
            {}

            template<class U>
            void push(U&& item) {
                const auto writeIndex = m_queue->m_writer.writeIndex.load(std::memory_order_relaxed);
                const auto nextWriteIndex = (writeIndex == m_queue->base::capacity - 1) ? 0 : writeIndex + 1;

                while (nextWriteIndex == m_queue->m_writer.readIndex) {
                    m_queue->m_writer.readIndex =  m_queue->m_reader.readIndex.load(std::memory_order_acquire);
                }

                m_queue->data()[writeIndex + m_queue->base::padding] = std::forward<U>(item);
                m_queue->m_writer.writeIndex.store(nextWriteIndex, std::memory_order_release);
            }

            template<class U>
            void force_push(U&& item) {
                const auto writeIndex = m_queue->m_writer.writeIndex.load(std::memory_order_relaxed);
                const auto nextWriteIndex = (writeIndex == m_queue->base::capacity - 1) ? 0 : writeIndex + 1;

                if (nextWriteIndex == m_queue->m_writer.readIndex) {
                    m_queue->m_writer.readIndex = m_queue->m_reader.readIndex.load(std::memory_order_acquire);

                    if (nextWriteIndex == m_queue->m_writer.readIndex) {
                        const auto nextReadIndex = m_queue->m_writer.readIndex ==m_queue->base::capacity - 1 ? 0 : m_queue->m_writer.readIndex + 1;

                        m_queue->m_reader.readIndex.store(nextReadIndex, std::memory_order_release);
                        m_queue->m_writer.readIndex = nextReadIndex;
                    }
                }

                m_queue->data()[writeIndex + m_queue->base::padding] = std::forward<U>(item);
                m_queue->m_writer.writeIndex.store(nextWriteIndex, std::memory_order_release);
            }

            template<class U>
            bool try_push(U&& item) {
                const auto writeIndex = m_queue->m_writer.writeIndex.load(std::memory_order_relaxed);
                const auto nextWriteIndex = (writeIndex == m_queue->base::capacity - 1) ? 0 : writeIndex + 1;

                if (nextWriteIndex == m_queue->m_writer.readIndex) {
                    m_queue->m_writer.readIndex =  m_queue->m_reader.readIndex.load(std::memory_order_acquire);

                    if (nextWriteIndex == m_queue->m_writer.readIndex) {
                        return false;
                    }
                }
                m_queue->data()[writeIndex + m_queue->base::padding] = std::forward<U>(item);
                m_queue->m_writer.writeIndex.store(nextWriteIndex, std::memory_order_release);
                return true;
            }

            template<class... Args>
            void emplace(Args&&... args)
            requires std::constructible_from<T, Args...> {
                T tmp(std::forward<Args>(args)...);
                push(std::move(tmp));
            }

            template<class... Args>
            void force_emplace(Args&&... args)
            requires std::constructible_from<T, Args...> {
                T tmp(std::forward<Args>(args)...);
                force_push(std::move(tmp));
            }

            [[nodiscard]] T pop() {
                const auto readIndex = m_queue->m_reader.readIndex.load(std::memory_order_relaxed);

                while (readIndex == m_queue->m_reader.writeIndex) {
                    m_queue->m_reader.writeIndex = m_queue->m_writer.writeIndex.load(std::memory_order_acquire);
                }

                T item = std::move(m_queue->data()[readIndex + m_queue->base::padding]);
                const auto nextReadIndex = (readIndex == m_queue->base::capacity - 1) ? 0 : readIndex + 1;

                m_queue->m_reader.readIndex.store(nextReadIndex, std::memory_order_release);
                return item;
            }

            [[nodiscard]] std::optional<T> try_pop() {
                const auto readIndex = m_queue->m_reader.readIndex.load(std::memory_order_relaxed);

                if (readIndex == m_queue->m_reader.writeIndex) {
                    m_queue->m_reader.writeIndex = m_queue->m_writer.writeIndex.load(std::memory_order_acquire);

                    if (readIndex == m_queue->m_reader.writeIndex) {
                        return std::nullopt;
                    }
                }

                T item = std::move(m_queue->data()[readIndex + m_queue->base::padding]);
                const auto nextReadIndex = (readIndex == m_queue->base::capacity - 1) ? 0 : readIndex + 1;

                m_queue->m_reader.readIndex.store(nextReadIndex, std::memory_order_release);
                return item;
            }
        private:
            queue* m_queue;
        };

        std::vector<T, Allocator>& data() { return base::data; }

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
    };
}
