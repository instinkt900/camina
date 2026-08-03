#pragma once

/**
 * @file
 * @brief What the cooker produced, and what it produced each thing from.
 *
 * The manifest does two jobs, and they belong together because both need the
 * same record.
 *
 * The cooker reads it to decide what to skip. An entry stores a hash of every
 * input, so a second run over an unchanged tree cooks nothing. An entry that
 * lists more than one input recooks when any of them changes, which is how a
 * shader that includes a shared header stays correct.
 *
 * The runtime reads it to find an asset. An asset names another asset by GUID,
 * because M4.1 made a GUID the identity that survives a rename. But something
 * has to start the chain, and code cannot hold a GUID it never saw. So the
 * manifest also maps a source path to a GUID. A caller looks a path up once at
 * startup and holds the handle. Only that first lookup depends on a path, and
 * nothing an asset stores does.
 */

#include "core/guid.h"
#include "reflect/reflect.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace engine::assets {

    /// @brief The file the cooker writes, inside the cooked directory.
    inline constexpr const char* kManifestFile = "manifest.json";

    /**
     * @brief One file the cooker wrote, and the identity it goes by.
     *
     * A source file with one rule produces one of these, and it carries the
     * source file's own GUID. A glTF file produces several, one for each mesh
     * and later one for each material, and each of those carries a GUID that
     * `Guid::derive` worked out from the source GUID.
     */
    struct ManifestOutput {
        /// @brief The cooked path, relative to the cooked root, with forward slashes.
        std::string cooked;

        /// @brief The identity a scene, a prefab, or another asset stores.
        Guid guid;
    };

    /**
     * @brief One source asset, what it produced, and what it was built from.
     */
    struct ManifestEntry {
        /// @brief The source path, relative to the content root, with forward slashes.
        std::string source;

        /// @brief The identity, from the source file's `.meta` sidecar.
        Guid guid;

        /**
         * @brief Every file this source produced.
         *
         * Most rules write one. The glTF rule writes one for each mesh, because
         * a prefab has to name a single mesh and not the file it came from.
         */
        std::vector<ManifestOutput> outputs;

        /// @brief Every source file these outputs were built from, @ref source first.
        std::vector<std::string> inputs;

        /// @brief The hash of every input, folded together in order.
        std::uint64_t hash = 0;
    };

    /**
     * @brief Every cooked output from one run of the cooker.
     */
    struct Manifest {
        /// @brief The entries, in the order the cooker wrote them.
        std::vector<ManifestEntry> entries;
    };

    /**
     * @brief Hashes a file's bytes.
     * @param path The file to read.
     * @param out The hash. It is not written when the file cannot be read.
     * @return True when the file was read.
     */
    [[nodiscard]] bool hash_file(const std::filesystem::path& path, std::uint64_t& out);

    /**
     * @brief Folds the hash of every input together, in order.
     *
     * The order matters, so two entries with the same files in another order
     * give another answer. That is what makes a reordered input list count as
     * a change.
     *
     * @param root The content root the input paths are relative to.
     * @param inputs The input paths.
     * @param out The combined hash.
     * @return True when every input was read.
     */
    [[nodiscard]] bool hash_inputs(const std::filesystem::path& root,
                                   const std::vector<std::string>& inputs, std::uint64_t& out);

    /**
     * @brief Finds the entry for a source path.
     * @param manifest The manifest to search.
     * @param source The source path, relative to the content root.
     * @return The entry, or nullptr when nothing cooked that path.
     */
    [[nodiscard]] const ManifestEntry* find_by_source(const Manifest& manifest,
                                                      std::string_view source);

    /**
     * @brief Finds the cooked output for a GUID.
     *
     * This searches the outputs rather than the entries, because a GUID names
     * one cooked file. A glTF file holds several meshes, and asking for the
     * entry would say which glTF a mesh came from and not which mesh it is.
     *
     * @param manifest The manifest to search.
     * @param guid The identity to look for.
     * @return The output, or nullptr when nothing cooked that identity.
     */
    [[nodiscard]] const ManifestOutput* find_by_guid(const Manifest& manifest, Guid guid);

    /**
     * @brief Finds the entry that produced a GUID.
     * @param manifest The manifest to search.
     * @param guid The identity to look for.
     * @return The entry, or nullptr when nothing cooked that identity.
     */
    [[nodiscard]] const ManifestEntry* find_source_of(const Manifest& manifest, Guid guid);

    /**
     * @brief Whether an entry still matches what is on disk.
     *
     * An entry is stale when an input changed, when an input is gone, or when
     * the cooked file is missing. A stale entry has to be cooked again.
     *
     * @param entry The entry to check.
     * @param source_root The content root the inputs are relative to.
     * @param cooked_root The cooked root the output is relative to.
     * @return True when the cooker can skip this entry.
     */
    [[nodiscard]] bool is_fresh(const ManifestEntry& entry,
                                const std::filesystem::path& source_root,
                                const std::filesystem::path& cooked_root);

    /**
     * @brief Turns a path into the form the manifest stores.
     *
     * The manifest holds forward slashes on both platforms, so a directory
     * cooked on Windows reads the same on Linux.
     *
     * @param path The path, relative to a content root.
     * @return The text form.
     */
    [[nodiscard]] std::string manifest_path(const std::filesystem::path& path);

    /**
     * @brief Reads the manifest from a cooked directory.
     * @param cooked_root The cooked directory.
     * @param out The manifest to fill. It is left empty when there is no file.
     * @return True when the file was there and it parsed.
     */
    [[nodiscard]] bool load_manifest(const std::filesystem::path& cooked_root, Manifest& out);

    /**
     * @brief Writes the manifest into a cooked directory.
     * @param cooked_root The cooked directory. It must exist.
     * @param manifest The manifest to write.
     * @return True when the file was written.
     */
    [[nodiscard]] bool save_manifest(const std::filesystem::path& cooked_root,
                                     const Manifest& manifest);

} // namespace engine::assets

/// @brief Describes one manifest entry, so reflect/ reads and writes it.
template <>
struct engine::reflect::Describe<engine::assets::ManifestEntry> {
    /// @brief The type name a document stores.
    static constexpr const char* name = "ManifestEntry";

    /// @brief The fields, in the order a document holds them.
    /// @return The field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(ENGINE_FIELD(engine::assets::ManifestEntry, source),
                               ENGINE_FIELD(engine::assets::ManifestEntry, guid),
                               ENGINE_FIELD(engine::assets::ManifestEntry, outputs),
                               ENGINE_FIELD(engine::assets::ManifestEntry, inputs),
                               ENGINE_FIELD(engine::assets::ManifestEntry, hash));
    }
};

/// @brief Describes one cooked output, so reflect/ reads and writes it.
template <>
struct engine::reflect::Describe<engine::assets::ManifestOutput> {
    /// @brief The type name a document stores.
    static constexpr const char* name = "ManifestOutput";

    /// @brief The fields, in the order a document holds them.
    /// @return The field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(ENGINE_FIELD(engine::assets::ManifestOutput, cooked),
                               ENGINE_FIELD(engine::assets::ManifestOutput, guid));
    }
};

/// @brief Describes the manifest, so reflect/ reads and writes it.
template <>
struct engine::reflect::Describe<engine::assets::Manifest> {
    /// @brief The type name a document stores.
    static constexpr const char* name = "Manifest";

    /// @brief The one field.
    /// @return The field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(ENGINE_FIELD(engine::assets::Manifest, entries));
    }
};
