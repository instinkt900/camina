#pragma once

/**
 * @file
 * @brief A linear allocator for short-lived allocations.
 */

#include <cstddef>
#include <new>
#include <type_traits>

namespace engine {

    /**
     * @brief A linear allocator over one fixed block.
     *
     * Allocation moves a pointer forward. There is no per-object free. reset()
     * releases everything at once.
     *
     * The frame arena uses this. Reset it at the top of each frame, then allocate
     * scratch data with no bookkeeping and no fragmentation.
     *
     * @warning The arena never calls a destructor. Store only trivially
     *          destructible types. allocate_n() enforces this at compile time.
     */
    class Arena {
    public:
        /**
         * @brief Allocates the backing block.
         * @param capacity Size of the block in bytes. The block is aligned to a
         *                 cache line.
         */
        explicit Arena(std::size_t capacity);
        ~Arena();

        Arena(const Arena&) = delete;
        Arena& operator=(const Arena&) = delete;

        /// @brief Takes ownership of the block held by @p other.
        /// @param other The arena to move from. It is left empty.
        Arena(Arena&& other) noexcept;

        /// @brief Releases the current block and takes the one held by @p other.
        /// @param other The arena to move from. It is left empty.
        /// @return A reference to this arena.
        Arena& operator=(Arena&& other) noexcept;

        /**
         * @brief Returns a block of the requested size and alignment.
         * @param size Number of bytes required.
         * @param alignment Required alignment. Must be a power of two.
         * @return A pointer to the block, or nullptr when the arena is full. The
         *         caller must check the result.
         */
        [[nodiscard]] void* allocate(std::size_t size, std::size_t alignment);

        /**
         * @brief Returns storage for @p count objects of type T.
         *
         * The objects are not constructed. The memory holds whatever the previous
         * user of this region left behind.
         *
         * @tparam T Element type. Must be trivially destructible.
         * @param count Number of elements.
         * @return A pointer to the storage, or nullptr when the arena is full.
         */
        template <typename T>
        [[nodiscard]] T* allocate_n(std::size_t count) {
            static_assert(std::is_trivially_destructible_v<T>,
                          "The arena never runs a destructor. Store only trivially "
                          "destructible types.");
            return static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
        }

        /// @brief Releases every allocation. The block stays owned by the arena.
        void reset() noexcept;

        /// @brief Bytes handed out since the last reset.
        /// @return The current offset into the block.
        [[nodiscard]] std::size_t used() const noexcept { return offset_; }

        /// @brief Total size of the block.
        /// @return The capacity in bytes.
        [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

        /// @brief Bytes still available before the arena is full.
        /// @return The remaining capacity in bytes.
        [[nodiscard]] std::size_t remaining() const noexcept { return capacity_ - offset_; }

        /**
         * @brief The largest value that used() reached since construction.
         *
         * Read this to size the arena from measurement instead of guesswork. reset()
         * does not clear it.
         *
         * @return The high water mark in bytes.
         */
        [[nodiscard]] std::size_t high_water() const noexcept { return high_water_; }

    private:
        std::byte* base_ = nullptr;
        std::size_t capacity_ = 0;
        std::size_t offset_ = 0;
        std::size_t high_water_ = 0;
    };

} // namespace engine
