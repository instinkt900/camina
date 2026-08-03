#pragma once

/**
 * @file
 * @brief Reads a cooked content directory.
 *
 * This is the runtime half of the cooker. It opens a cooked directory, reads
 * the manifest, and hands out the bytes of a cooked asset.
 *
 * A caller starts from a source path, because code cannot hold a GUID it never
 * saw. `find()` turns that path into an entry once, and everything after works
 * from the GUID in that entry. An asset that names another asset still stores
 * only a GUID, so a rename inside the content tree changes nothing.
 */

#include "assets/database.h"
#include "assets/manifest.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace engine::assets {

    /**
     * @brief One cooked content directory, open for reading.
     *
     * @code
     * engine::assets::Content content;
     * if (content.open(directory)) {
     *     std::vector<std::uint32_t> words;
     *     content.read_words("cube.vert", words);
     * }
     * @endcode
     */
    class Content {
    public:
        /**
         * @brief Opens a cooked directory and reads its manifest.
         * @param cooked_root The directory the cooker wrote.
         * @return True when the manifest was there and it parsed.
         */
        [[nodiscard]] bool open(const std::filesystem::path& cooked_root);

        /**
         * @brief Finds what the cooker made from a source path.
         * @param source The source path, relative to the content root, with
         * forward slashes. For a shader, name the GLSL file and not the `.spv`.
         * @return The entry, or nullptr when the manifest has no such path.
         */
        [[nodiscard]] const ManifestEntry* find(std::string_view source) const;

        /**
         * @brief Reads the bytes of a cooked asset.
         * @param entry The entry to read, from find().
         * @param out The bytes.
         * @return True when the file was read.
         */
        [[nodiscard]] bool read_bytes(const ManifestEntry& entry,
                                      std::vector<std::byte>& out) const;

        /**
         * @brief Reads a cooked asset as 32-bit words, which is what SPIR-V is.
         *
         * This reports a file whose length is not a multiple of four, because
         * that is not a SPIR-V module and the driver would reject it later
         * with a message that names nothing useful.
         *
         * @param source The source path, as find() takes it.
         * @param out The words.
         * @return True when the asset was there and it read.
         */
        [[nodiscard]] bool read_words(std::string_view source,
                                      std::vector<std::uint32_t>& out) const;

        /// @brief The directory this was opened on.
        /// @return The cooked root.
        [[nodiscard]] const std::filesystem::path& root() const { return root_; }

        /// @brief The manifest the cooker wrote.
        /// @return The manifest.
        [[nodiscard]] const Manifest& manifest() const { return manifest_; }

        /// @brief The database that holds what this content loaded.
        /// @return The database.
        [[nodiscard]] AssetDatabase& database() { return database_; }

    private:
        std::filesystem::path root_;
        Manifest manifest_;
        AssetDatabase database_;
    };

} // namespace engine::assets
