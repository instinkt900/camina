#include "core/arena.h"

#include "core/assert.h"

#include <algorithm>
#include <utility>

namespace engine {

    namespace {

        /// The whole block is aligned to a cache line, so the first allocation of a
        /// frame never straddles one. Every later allocation aligns relative to this.
        constexpr std::size_t kBlockAlignment = 64;

        std::size_t align_up(std::size_t value, std::size_t alignment) {
            return (value + alignment - 1U) & ~(alignment - 1U);
        }

        bool is_power_of_two(std::size_t value) {
            return value != 0 && (value & (value - 1U)) == 0;
        }

    } // namespace

    Arena::Arena(std::size_t capacity)
        : base_(static_cast<std::byte*>(
              ::operator new(capacity, std::align_val_t{ kBlockAlignment })))
        , capacity_(capacity) {}

    Arena::~Arena() {
        if (base_ != nullptr) {
            ::operator delete(base_, std::align_val_t{ kBlockAlignment });
        }
    }

    Arena::Arena(Arena&& other) noexcept
        : base_(std::exchange(other.base_, nullptr))
        , capacity_(std::exchange(other.capacity_, 0))
        , offset_(std::exchange(other.offset_, 0))
        , high_water_(std::exchange(other.high_water_, 0)) {}

    Arena& Arena::operator=(Arena&& other) noexcept {
        if (this != &other) {
            if (base_ != nullptr) {
                ::operator delete(base_, std::align_val_t{ kBlockAlignment });
            }
            base_ = std::exchange(other.base_, nullptr);
            capacity_ = std::exchange(other.capacity_, 0);
            offset_ = std::exchange(other.offset_, 0);
            high_water_ = std::exchange(other.high_water_, 0);
        }
        return *this;
    }

    void* Arena::allocate(std::size_t size, std::size_t alignment) {
        ENGINE_ASSERT(is_power_of_two(alignment), "The alignment must be a power of two.");

        const std::size_t aligned = align_up(offset_, alignment);
        if (aligned + size > capacity_) {
            return nullptr;
        }

        std::byte* result = base_ + aligned;
        offset_ = aligned + size;
        high_water_ = std::max(high_water_, offset_);
        return result;
    }

    void Arena::reset() noexcept {
        offset_ = 0;
    }

} // namespace engine
