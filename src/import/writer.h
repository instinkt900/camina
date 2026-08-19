#pragma once

/**
 * @file
 * @brief Where a rule puts the asset it finished.
 *
 * A rule used to open the destination file itself. It writes through this
 * instead, so the same rule can put its result on disk for the cooker or in
 * memory for the editor. That is what lets the editor import an asset without
 * cooking a tree, and it is what keeps the two on one code path: the bytes come
 * from the same rule either way. See `DESIGN.md` §10 M13.
 *
 * A rule names its output by the cooked path relative to the cooked root, which
 * is what the manifest stores. So a rule needs no idea where that root is, or
 * whether there is one.
 */

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace engine::import {

    /// @brief Takes the finished bytes of one cooked asset.
    class Writer {
    public:
        Writer() = default;
        virtual ~Writer() = default;

        Writer(const Writer&) = delete;
        Writer& operator=(const Writer&) = delete;
        Writer(Writer&&) = delete;
        Writer& operator=(Writer&&) = delete;

        /**
         * @brief Takes one finished asset.
         * @param cooked The cooked path, relative to the cooked root.
         * @param bytes The whole file.
         * @return True when it was stored.
         */
        [[nodiscard]] virtual bool write(const std::filesystem::path& cooked,
                                         std::span<const std::byte> bytes) = 0;
    };

    /**
     * @brief A writer that puts each asset in a file under a root.
     *
     * This is what the cooker uses, and it makes the directories a cooked path
     * needs. Its output is the cooked tree the runtime reads.
     */
    class FileWriter : public Writer {
    public:
        /// @brief Makes a writer over a cooked root.
        /// @param root The cooked directory to write under.
        explicit FileWriter(std::filesystem::path root)
            : root_(std::move(root)) {}

        /**
         * @brief Writes one asset to disk, making its directory first.
         * @param cooked The cooked path, relative to the root.
         * @param bytes The whole file.
         * @return True when the file was written.
         */
        [[nodiscard]] bool write(const std::filesystem::path& cooked,
                                 std::span<const std::byte> bytes) override;

        /// @brief The root this writes under.
        /// @return The cooked root.
        [[nodiscard]] const std::filesystem::path& root() const { return root_; }

    private:
        std::filesystem::path root_;
    };

    /**
     * @brief A writer that keeps each asset in memory.
     *
     * This is what the editor uses. Nothing reaches the file system, so
     * importing an asset leaves no cooked tree behind and needs no place to put
     * one.
     */
    class MemoryWriter : public Writer {
    public:
        /**
         * @brief Keeps one asset.
         * @param cooked The cooked path, which is the key.
         * @param bytes The whole file.
         * @return True, always. Nothing here can fail.
         */
        [[nodiscard]] bool write(const std::filesystem::path& cooked,
                                 std::span<const std::byte> bytes) override;

        /**
         * @brief What was written, keyed by cooked path in the manifest form.
         * @return The files.
         */
        [[nodiscard]] const std::map<std::string, std::vector<std::byte>>& files() const {
            return files_;
        }

        /// @brief Forgets everything written so far.
        void clear() { files_.clear(); }

    private:
        std::map<std::string, std::vector<std::byte>> files_;
    };

    /**
     * @brief Writes one cooked file made of a fixed-size header and a payload.
     *
     * Most cooked formats are exactly that shape, and a Writer takes one span.
     * So this joins the two rather than each rule doing it.
     *
     * @tparam Header The header struct, which must be trivially copyable.
     * @param writer Where the file goes.
     * @param cooked The cooked path, relative to the cooked root.
     * @param header The header, written first.
     * @param payload The bytes after it.
     * @return True when the writer took it.
     */
    template <typename Header>
    [[nodiscard]] bool write_with_header(Writer& writer, const std::filesystem::path& cooked,
                                         const Header& header,
                                         std::span<const std::byte> payload) {
        static_assert(std::is_trivially_copyable_v<Header>,
                      "a cooked header is copied byte for byte");
        std::vector<std::byte> whole(sizeof(Header) + payload.size());
        std::memcpy(whole.data(), &header, sizeof(Header));
        if (!payload.empty()) {
            std::memcpy(whole.data() + sizeof(Header), payload.data(), payload.size());
        }
        return writer.write(cooked, whole);
    }

} // namespace engine::import
