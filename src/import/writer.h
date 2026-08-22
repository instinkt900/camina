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
         * @brief Takes one finished asset, and holds it back.
         *
         * **Nothing a rule writes is visible until commit().** A rule that
         * writes several outputs can fail on the third, and publishing the
         * first two would leave a cooked tree holding parts of one asset from
         * two versions of its source. See issue #104.
         *
         * @param cooked The cooked path, relative to the cooked root.
         * @param bytes The whole file.
         * @return True when it was staged.
         */
        [[nodiscard]] virtual bool write(const std::filesystem::path& cooked,
                                         std::span<const std::byte> bytes) = 0;

        /**
         * @brief Publishes everything staged since the last commit or discard.
         *
         * @return True when every staged output was published. False leaves the
         * writer holding whatever it could not publish, and the caller should
         * treat the asset as failed.
         */
        [[nodiscard]] virtual bool commit() = 0;

        /**
         * @brief Throws away everything staged since the last commit or discard.
         *
         * Safe to call when nothing is staged, which is what lets a caller
         * discard on every failure path without asking whether a rule got as
         * far as writing anything.
         */
        virtual void discard() = 0;
    };

    /**
     * @brief Discards what a rule staged unless somebody commits it.
     *
     * The cooker has four ways to fail after a rule starts writing, and one of
     * them is a step that runs after the rule returned successfully. A guard
     * costs nothing and cannot be forgotten on a path somebody adds later,
     * which is how issue #104 stayed true for two rules and three releases.
     *
     * @code
     * StagedWrites staged(writer);
     * if (!cook_one(...)) {
     *     return Outcome::Failed;  // the guard discards
     * }
     * if (!staged.commit()) {
     *     return Outcome::Failed;
     * }
     * @endcode
     */
    class StagedWrites {
    public:
        /// @brief Guards @p writer until commit() or destruction.
        /// @param writer The writer to discard from. Held, not owned.
        explicit StagedWrites(Writer& writer)
            : writer_(&writer) {}

        ~StagedWrites() {
            if (writer_ != nullptr) {
                writer_->discard();
            }
        }

        StagedWrites(const StagedWrites&) = delete;
        StagedWrites& operator=(const StagedWrites&) = delete;
        StagedWrites(StagedWrites&&) = delete;
        StagedWrites& operator=(StagedWrites&&) = delete;

        /**
         * @brief Publishes the staged outputs and releases the guard.
         *
         * The guard is released whether or not the publish worked, because a
         * commit that failed part way has already moved some outputs and
         * discarding the rest would not put those back.
         *
         * @return What Writer::commit() returned.
         */
        [[nodiscard]] bool commit() {
            Writer* const writer = writer_;
            writer_ = nullptr;
            return writer->commit();
        }

    private:
        Writer* writer_ = nullptr;
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

        /**
         * @brief Renames every staged file onto its final path.
         *
         * A rename within one directory is atomic, so a reader sees the old
         * file or the new one and never a half-written one. The set is not
         * atomic as a group, and it does not need to be: nothing renames until
         * every part of the asset has been written, so the window where the
         * group is half published is a few renames wide rather than a whole
         * import wide.
         *
         * @return True when every staged file was renamed.
         */
        [[nodiscard]] bool commit() override;

        /// @brief Deletes the staged files, leaving the cooked tree alone.
        void discard() override;

        /// @brief The root this writes under.
        /// @return The cooked root.
        [[nodiscard]] const std::filesystem::path& root() const { return root_; }

    private:
        /// One staged output: where it is now, and where it goes.
        struct Staged {
            std::filesystem::path temporary; ///< The file on disk right now.
            std::filesystem::path final;     ///< Where commit() renames it to.
        };

        std::filesystem::path root_;
        std::vector<Staged> staged_;
        /// Makes each staged name unique inside one asset, because a rule may
        /// write the same cooked path twice and the second must not land on the
        /// first's temporary file.
        std::size_t next_ = 0;
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

        /// @brief Moves the staged assets into ::files().
        /// @return True, always. Nothing here can fail.
        [[nodiscard]] bool commit() override;

        /// @brief Forgets the staged assets, leaving ::files() alone.
        void discard() override;

        /**
         * @brief What was committed, keyed by cooked path in the manifest form.
         *
         * An asset that was written and not committed does not appear here, so
         * a failed import leaves no half of one behind for the editor to read.
         *
         * @return The files.
         */
        [[nodiscard]] const std::map<std::string, std::vector<std::byte>>& files() const {
            return files_;
        }

        /// @brief Forgets everything, committed and staged alike.
        void clear() {
            files_.clear();
            staged_.clear();
        }

    private:
        std::map<std::string, std::vector<std::byte>> files_;
        std::map<std::string, std::vector<std::byte>> staged_;
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
