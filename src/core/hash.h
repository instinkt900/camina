#pragma once

/**
 * @file
 * @brief A content hash that gives the same answer everywhere.
 *
 * The cooker manifest stores a hash of every input, and it compares that hash
 * on the next run to decide what to cook again. Two machines therefore have to
 * agree on the answer, and so do two builds of the same machine.
 *
 * `std::hash` cannot do this. The standard lets an implementation salt it, and
 * libstdc++ and the MSVC library already disagree for the same bytes. A cooked
 * directory that moves between a Linux machine and a Windows machine would
 * then recook everything, or worse, skip an asset that did change.
 *
 * FNV-1a is the answer here because it is ten lines, it needs no dependency,
 * and it is a fixed algorithm rather than an implementation detail. It is not
 * a cryptographic hash. Nothing here defends against a file somebody changed
 * on purpose to collide.
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace engine {

    /// @brief The FNV-1a starting value for the 64-bit variant.
    inline constexpr std::uint64_t kHashOffsetBasis = 0xcbf29ce484222325ULL;

    /// @brief The FNV-1a multiplier for the 64-bit variant.
    inline constexpr std::uint64_t kHashPrime = 0x100000001b3ULL;

    /**
     * @brief Hashes bytes, continuing from an earlier result.
     *
     * Pass the result of one call as @p seed to the next, and the answer is
     * the same as hashing the whole run at once. That lets a caller hash a
     * large file one block at a time.
     *
     * @param bytes The bytes to fold in.
     * @param seed Where to start. Use kHashOffsetBasis for the first call.
     * @return The hash.
     */
    [[nodiscard]] constexpr std::uint64_t hash_bytes(std::span<const std::byte> bytes,
                                                     std::uint64_t seed = kHashOffsetBasis) {
        std::uint64_t value = seed;
        for (const std::byte byte : bytes) {
            value ^= static_cast<std::uint64_t>(byte);
            value *= kHashPrime;
        }
        return value;
    }

    /**
     * @brief Hashes text.
     * @param text The text to hash.
     * @param seed Where to start. Use kHashOffsetBasis for the first call.
     * @return The hash.
     */
    [[nodiscard]] constexpr std::uint64_t hash_text(std::string_view text,
                                                    std::uint64_t seed = kHashOffsetBasis) {
        std::uint64_t value = seed;
        for (const char letter : text) {
            value ^= static_cast<std::uint64_t>(static_cast<unsigned char>(letter));
            value *= kHashPrime;
        }
        return value;
    }

} // namespace engine
