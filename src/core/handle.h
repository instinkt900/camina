#pragma once

/**
 * @file
 * @brief Generational handles for pooled resources.
 */

#include <cstdint>
#include <functional>

/// @brief Camina Engine. The project is called Camina, the namespace is engine.
namespace engine {

    /**
     * @brief A generational handle into a pool.
     *
     * The low 32 bits hold an index into a dense array. The high 32 bits hold a
     * generation counter that the owning pool increments on each free. A stale
     * handle therefore compares unequal to the live handle at the same index, so a
     * use-after-free returns null instead of the wrong object.
     *
     * The Tag parameter makes handles of different kinds distinct types. A texture
     * handle then cannot pass as a buffer handle by mistake:
     *
     * @code
     * struct TextureTag {};
     * using TextureHandle = engine::Handle<TextureTag>;
     * @endcode
     *
     * This type stays trivially copyable and 8 bytes wide, so it can cross the
     * future C plugin ABI unchanged. See rule 4.2 in DESIGN.md.
     *
     * @tparam Tag An empty type that distinguishes one kind of handle from another.
     */
    template <typename Tag>
    struct Handle {
        /// @brief Width of the index field in bits.
        static constexpr std::uint32_t kIndexBits = 32U;
        /// @brief Mask that isolates the index field.
        static constexpr std::uint64_t kIndexMask = (1ULL << kIndexBits) - 1ULL;

        /// @brief The packed index and generation. Zero means no handle.
        std::uint64_t value = 0;

        /// @brief Reports whether the handle refers to anything.
        /// @return True when the handle is not the default.
        [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }

        /// @brief The slot this handle refers to.
        /// @return The index field.
        [[nodiscard]] constexpr std::uint32_t index() const noexcept {
            return static_cast<std::uint32_t>(value & kIndexMask);
        }

        /// @brief How many times the slot had been reused when this handle was made.
        /// @return The generation field.
        [[nodiscard]] constexpr std::uint32_t generation() const noexcept {
            return static_cast<std::uint32_t>(value >> kIndexBits);
        }

        /**
         * @brief Packs an index and a generation into a handle.
         * @param index The slot in the owning pool.
         * @param generation The pool's reuse count for that slot.
         * @return The packed handle.
         */
        [[nodiscard]] static constexpr Handle make(std::uint32_t index,
                                                   std::uint32_t generation) noexcept {
            return Handle{ (static_cast<std::uint64_t>(generation) << kIndexBits) |
                           static_cast<std::uint64_t>(index) };
        }

        /// @brief Compares two handles for equality.
        /// @param a The first handle.
        /// @param b The second handle.
        /// @return True when both the index and the generation match.
        friend constexpr bool operator==(Handle a, Handle b) noexcept { return a.value == b.value; }

        /// @brief Compares two handles for inequality.
        /// @param a The first handle.
        /// @param b The second handle.
        /// @return True when the index or the generation differs.
        friend constexpr bool operator!=(Handle a, Handle b) noexcept { return a.value != b.value; }
    };

} // namespace engine

/// @cond
// Hash support, so a handle can key an unordered container. Doxygen 1.9.8 cannot
// resolve a specialization of a std template and reports an internal inconsistency,
// so it is hidden from the docs rather than documented.
template <typename Tag>
struct std::hash<engine::Handle<Tag>> {
    std::size_t operator()(engine::Handle<Tag> handle) const noexcept {
        return std::hash<std::uint64_t>{}(handle.value);
    }
};
/// @endcond
