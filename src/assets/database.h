#pragma once

/**
 * @file
 * @brief Handles for loaded assets, and the database that resolves a GUID.
 *
 * A component names an asset by GUID, because a GUID survives a rename. A
 * renderer cannot chase a GUID through a hash map on every draw, so it holds a
 * handle instead. This database converts one to the other.
 *
 * The conversion happens once, and the handle stays valid after that. A GUID
 * gets its slot the first time somebody asks for it, before the asset loads.
 * Loading fills the slot the handle already points at. That is what lets M4.5
 * replace an asset while the program runs, with no handle to fix up.
 *
 * Nothing here reads a file. The cooker (#36) and the loaders fill the
 * database, and a test fills it by hand.
 */

#include "core/guid.h"
#include "core/handle.h"
#include "core/log.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <utility>

/// @brief Asset identity, the runtime asset database, and hot reload.
namespace engine::assets {

    /**
     * @brief A handle to a loaded asset of one type.
     *
     * The asset type is the tag, so a mesh handle cannot pass as a texture
     * handle. See engine::Handle.
     *
     * @tparam T The asset type.
     */
    template <typename T>
    using AssetHandle = Handle<T>;

    /// @brief How far along one asset is.
    enum class AssetState : std::uint8_t {
        Unloaded, ///< The slot exists and nothing has filled it yet.
        Loaded,   ///< The slot holds the asset.
        Failed,   ///< Loading was tried and it did not work.
    };

    /**
     * @brief Turns a state into text for a log line.
     * @param state The state to name.
     * @return A static string.
     */
    [[nodiscard]] const char* state_name(AssetState state);

    /// @cond
    // The storage base exists so one database can hold a pool for each asset
    // type. It carries no interface of its own, and no caller names it.
    namespace detail {

        class Storage {
        public:
            Storage() = default;
            Storage(const Storage&) = delete;
            Storage(Storage&&) = delete;
            Storage& operator=(const Storage&) = delete;
            Storage& operator=(Storage&&) = delete;
            virtual ~Storage() = default;
        };

    } // namespace detail
    /// @endcond

    /**
     * @brief Every slot for one asset type, keyed by GUID.
     *
     * Reach this through AssetDatabase rather than building one. It is public
     * so the database can name it in a template.
     *
     * @tparam T The asset type. It must be default constructible, because an
     * empty slot holds the placeholder.
     */
    template <typename T>
    class AssetPool : public detail::Storage {
    public:
        /**
         * @brief Finds the slot for a GUID, and makes one when there is none.
         *
         * The handle stays valid for as long as the pool lives, so a caller can
         * resolve once and keep the result.
         *
         * @param guid The asset to name. A null GUID gets no slot.
         * @return The handle, or an invalid handle for a null GUID.
         */
        [[nodiscard]] AssetHandle<T> resolve(Guid guid) {
            if (!guid.valid()) {
                return AssetHandle<T>{};
            }
            if (const auto found = by_guid_.find(guid); found != by_guid_.end()) {
                return handle_at(found->second);
            }

            const std::size_t index = slots_.size();
            slots_.push_back(Slot{ .guid = guid });
            by_guid_.emplace(guid, index);
            return handle_at(index);
        }

        /**
         * @brief Puts an asset in the slot for a GUID and marks it loaded.
         *
         * Calling this a second time for the same GUID replaces the asset and
         * keeps the handle, which is the shape hot reload needs.
         *
         * @param guid The asset to fill.
         * @param value The asset.
         * @return The handle to the slot.
         */
        AssetHandle<T> store(Guid guid, T value) {
            const AssetHandle<T> handle = resolve(guid);
            if (!handle.valid()) {
                return handle;
            }
            Slot& slot = slots_[handle.index()];
            slot.value = std::move(value);
            slot.state = AssetState::Loaded;
            slot.reported = false;
            return handle;
        }

        /**
         * @brief Marks the slot for a GUID as failed, and says why.
         *
         * The database remembers the failure, so a caller that asks every frame
         * gets one log line rather than a stream of them.
         *
         * @param guid The asset that did not load.
         * @param reason What went wrong, for the log.
         * @return The handle to the slot.
         */
        AssetHandle<T> fail(Guid guid, std::string_view reason) {
            const AssetHandle<T> handle = resolve(guid);
            if (!handle.valid()) {
                return handle;
            }
            Slot& slot = slots_[handle.index()];
            slot.value = T{};
            slot.state = AssetState::Failed;
            slot.reported = false;
            ENGINE_LOG_ERROR("Asset {} did not load: {}", guid.to_text(), reason);
            return handle;
        }

        /// @brief How far along the asset behind a handle is.
        /// @param handle The handle to read.
        /// @return The state. An invalid or stale handle reads as Unloaded.
        [[nodiscard]] AssetState state(AssetHandle<T> handle) const {
            const Slot* slot = live_slot(handle);
            return slot == nullptr ? AssetState::Unloaded : slot->state;
        }

        /// @brief The GUID a handle names.
        /// @param handle The handle to read.
        /// @return The GUID, or the null GUID for an invalid or stale handle.
        [[nodiscard]] Guid guid_of(AssetHandle<T> handle) const {
            const Slot* slot = live_slot(handle);
            return slot == nullptr ? Guid{} : slot->guid;
        }

        /**
         * @brief The asset behind a handle, or the placeholder.
         *
         * This never fails and never returns null. An asset that is missing,
         * still loading, or broken gives the placeholder, so a caller draws
         * something wrong rather than ending the process. The first such call
         * for each slot writes one log line.
         *
         * @param handle The handle to read.
         * @return The asset, or the placeholder.
         */
        [[nodiscard]] const T& get(AssetHandle<T> handle) const {
            const Slot* slot = live_slot(handle);
            if (slot == nullptr) {
                return placeholder_;
            }
            if (slot->state == AssetState::Loaded) {
                return slot->value;
            }
            if (!slot->reported) {
                slot->reported = true;
                ENGINE_LOG_WARN("Asset {} is {}. Drawing the placeholder.",
                                slot->guid.to_text(), state_name(slot->state));
            }
            return placeholder_;
        }

        /**
         * @brief Sets what a caller gets for an asset that is not there.
         *
         * Give this something a person can see, such as a magenta texture or a
         * unit cube. The default is a value-initialized asset, which is quiet
         * and therefore easy to miss.
         *
         * @param value The placeholder.
         */
        void set_placeholder(T value) { placeholder_ = std::move(value); }

        /// @brief The placeholder this pool hands out.
        /// @return The placeholder.
        [[nodiscard]] const T& placeholder() const { return placeholder_; }

        /// @brief How many slots exist, loaded or not.
        /// @return The slot count.
        [[nodiscard]] std::size_t size() const { return slots_.size(); }

        /**
         * @brief Drops every slot.
         *
         * Every handle this pool gave out becomes stale, and reading one gives
         * the placeholder rather than whatever lands in the slot next.
         */
        void clear() {
            ++generation_;
            slots_.clear();
            by_guid_.clear();
        }

    private:
        struct Slot {
            Guid guid;
            T value{};
            AssetState state = AssetState::Unloaded;

            // Whether get() has already logged this slot. It is mutable because
            // it records what the log has said, not what the asset is, and a
            // caller with a const database still has to be able to read.
            mutable bool reported = false;
        };

        [[nodiscard]] AssetHandle<T> handle_at(std::size_t index) const {
            return AssetHandle<T>::make(static_cast<std::uint32_t>(index), generation_);
        }

        /// The slot a handle names, or nullptr when the handle is stale or invalid.
        [[nodiscard]] const Slot* live_slot(AssetHandle<T> handle) const {
            if (!handle.valid() || handle.generation() != generation_ ||
                handle.index() >= slots_.size()) {
                return nullptr;
            }
            return &slots_[handle.index()];
        }

        // The generation starts at 1, not at 0. A handle packs the index and
        // the generation, and Handle::valid() reads a zero pack as no handle,
        // so slot 0 of generation 0 would be indistinguishable from nothing.
        std::uint32_t generation_ = 1;

        // A deque, not a vector. get() returns a reference into a slot, and
        // resolve() adds slots. A vector moves every slot when it grows, which
        // would leave a caller holding a reference to freed memory. A deque
        // keeps a reference valid when it grows at the end.
        std::deque<Slot> slots_;
        std::unordered_map<Guid, std::size_t> by_guid_;
        T placeholder_{};
    };

    /**
     * @brief The asset database: one pool for each asset type.
     *
     * @code
     * engine::assets::AssetDatabase database;
     * const auto handle = database.resolve<Texture>(guid);
     * const Texture& texture = database.get(handle);
     * @endcode
     */
    class AssetDatabase {
    public:
        /**
         * @brief The pool for one asset type, made on the first call.
         * @tparam T The asset type.
         * @return The pool.
         */
        template <typename T>
        [[nodiscard]] AssetPool<T>& pool() {
            const std::type_index key{ typeid(T) };
            const auto found = pools_.find(key);
            if (found != pools_.end()) {
                // The map holds a pool of exactly this type under this key, so
                // the cast cannot land on another type.
                return static_cast<AssetPool<T>&>(*found->second);
            }
            auto created = std::make_unique<AssetPool<T>>();
            AssetPool<T>& pool = *created;
            pools_.emplace(key, std::move(created));
            return pool;
        }

        /// @brief Finds or makes the slot for a GUID. See AssetPool::resolve().
        /// @tparam T The asset type.
        /// @param guid The asset to name.
        /// @return The handle.
        template <typename T>
        [[nodiscard]] AssetHandle<T> resolve(Guid guid) {
            return pool<T>().resolve(guid);
        }

        /// @brief Puts an asset in the database. See AssetPool::store().
        /// @tparam T The asset type, deduced.
        /// @param guid The asset to fill.
        /// @param value The asset.
        /// @return The handle.
        template <typename T>
        AssetHandle<T> store(Guid guid, T value) {
            return pool<T>().store(guid, std::move(value));
        }

        /// @brief Reads an asset, or the placeholder. See AssetPool::get().
        /// @tparam T The asset type, deduced from the handle.
        /// @param handle The handle to read.
        /// @return The asset, or the placeholder.
        template <typename T>
        [[nodiscard]] const T& get(AssetHandle<T> handle) {
            return pool<T>().get(handle);
        }

        /// @brief How far along an asset is. See AssetPool::state().
        /// @tparam T The asset type, deduced from the handle.
        /// @param handle The handle to read.
        /// @return The state.
        template <typename T>
        [[nodiscard]] AssetState state(AssetHandle<T> handle) {
            return pool<T>().state(handle);
        }

        /// @brief How many asset types the database holds a pool for.
        /// @return The pool count.
        [[nodiscard]] std::size_t pool_count() const { return pools_.size(); }

    private:
        std::unordered_map<std::type_index, std::unique_ptr<detail::Storage>> pools_;
    };

} // namespace engine::assets
