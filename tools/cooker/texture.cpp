#include "texture.h"

#include "core/log.h"

#include <bc7enc.h>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace cooker {

    namespace {

        namespace as = engine::assets;

        constexpr std::size_t kChannels = 4;
        constexpr std::size_t kTexelsPerBlock =
            static_cast<std::size_t>(as::kBlockSize) * as::kBlockSize;
        constexpr std::size_t kBytesPerBlock = 16;
        constexpr float kByteMax = 255.0F;

        /// One mip level, held as 8-bit RGBA in the order the file will store it.
        struct Level {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::vector<std::uint8_t> texels;
        };

        /// A copy of the source that the encoder frees, so a return can be early.
        struct StbImage {
            stbi_uc* pixels = nullptr;
            int width = 0;
            int height = 0;

            ~StbImage() {
                if (pixels != nullptr) {
                    stbi_image_free(pixels);
                }
            }

            StbImage() = default;
            StbImage(const StbImage&) = delete;
            StbImage& operator=(const StbImage&) = delete;
            StbImage(StbImage&&) = delete;
            StbImage& operator=(StbImage&&) = delete;
        };

        [[nodiscard]] std::string lowered(std::string_view text) {
            std::string out;
            out.reserve(text.size());
            for (const char c : text) {
                out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
            }
            return out;
        }

        /**
         * The sRGB transfer function, and its inverse.
         *
         * These are what makes a mip chain correct. Averaging two sRGB bytes
         * gives the wrong answer, because the encoding is not linear in light.
         * Half black and half white averages to 188 in sRGB, not to 128, and a
         * chain built the naive way darkens as it goes down. That darkening is
         * the classic "distant textures look muddy" bug.
         */
        [[nodiscard]] float srgb_to_linear(float value) {
            constexpr float kKnee = 0.04045F;
            constexpr float kSlope = 12.92F;
            constexpr float kOffset = 0.055F;
            constexpr float kScale = 1.055F;
            constexpr float kGamma = 2.4F;
            if (value <= kKnee) {
                return value / kSlope;
            }
            return std::pow((value + kOffset) / kScale, kGamma);
        }

        [[nodiscard]] float linear_to_srgb(float value) {
            constexpr float kKnee = 0.0031308F;
            constexpr float kSlope = 12.92F;
            constexpr float kOffset = 0.055F;
            constexpr float kScale = 1.055F;
            constexpr float kGamma = 2.4F;
            if (value <= kKnee) {
                return value * kSlope;
            }
            return (kScale * std::pow(value, 1.0F / kGamma)) - kOffset;
        }

        [[nodiscard]] std::uint8_t to_byte(float value) {
            const float clamped = std::clamp(value, 0.0F, 1.0F);
            return static_cast<std::uint8_t>(std::lround(clamped * kByteMax));
        }

        /// The half-open source range one destination texel covers on one axis.
        struct Span {
            std::uint32_t first = 0;
            std::uint32_t last = 0; ///< One past the end. Always above `first`.
        };

        /**
         * Which source texels one destination texel averages.
         *
         * The range is worked out from the two sizes rather than fixed at two,
         * so an odd width drops no column. A 5 wide level halves to 2, and the
         * two destination texels cover 0 to 1 and 2 to 4.
         */
        [[nodiscard]] Span cover(std::uint32_t index, std::uint32_t source_size,
                                 std::uint32_t out_size) {
            const std::uint32_t first = index * source_size / out_size;
            const std::uint32_t last = (index + 1) * source_size / out_size;
            return Span{ .first = first, .last = std::max(last, first + 1) };
        }

        /// Alpha is the fourth channel, and it is the one the transfer function skips.
        constexpr std::size_t kAlphaChannel = 3;

        /**
         * Averages one box of source texels, in the space light adds in.
         *
         * The color channels of an sRGB texture go to linear first. Alpha never
         * does: alpha is coverage, it is already linear, and running it through
         * the transfer function would make every soft edge wrong.
         */
        [[nodiscard]] std::array<float, kChannels> box_average(const Level& source, Span across,
                                                               Span down, bool encoded) {
            std::array<float, kChannels> sum{};
            float count = 0.0F;

            for (std::uint32_t y = down.first; y < down.last; ++y) {
                for (std::uint32_t x = across.first; x < across.last; ++x) {
                    const std::size_t at =
                        ((static_cast<std::size_t>(y) * source.width) + x) * kChannels;
                    for (std::size_t c = 0; c < kChannels; ++c) {
                        const float value = static_cast<float>(source.texels[at + c]) / kByteMax;
                        sum[c] += encoded && c != kAlphaChannel ? srgb_to_linear(value) : value;
                    }
                    count += 1.0F;
                }
            }

            for (float& channel : sum) {
                channel /= count;
            }
            return sum;
        }

        /// Halves one level, honoring the color space the sidecar recorded.
        [[nodiscard]] Level halve(const Level& source, as::ColorSpace space) {
            Level out;
            out.width = std::max(source.width / 2, 1U);
            out.height = std::max(source.height / 2, 1U);
            out.texels.resize(static_cast<std::size_t>(out.width) * out.height * kChannels);

            const bool encoded = space == as::ColorSpace::Srgb;

            for (std::uint32_t y = 0; y < out.height; ++y) {
                const Span down = cover(y, source.height, out.height);
                for (std::uint32_t x = 0; x < out.width; ++x) {
                    const Span across = cover(x, source.width, out.width);
                    const std::array<float, kChannels> average =
                        box_average(source, across, down, encoded);

                    const std::size_t to =
                        ((static_cast<std::size_t>(y) * out.width) + x) * kChannels;
                    for (std::size_t c = 0; c < kChannels; ++c) {
                        out.texels[to + c] = to_byte(
                            encoded && c != kAlphaChannel ? linear_to_srgb(average[c])
                                                          : average[c]);
                    }
                }
            }
            return out;
        }

        /**
         * Copies one 4 by 4 block out of a level, for the encoder.
         *
         * A level smaller than a block still costs a whole block, so the texels
         * past the edge repeat the last real one. Repeating beats leaving them
         * zero: the encoder then spends no bits describing an edge that no
         * sampler ever reads.
         */
        void gather_block(const Level& level, std::uint32_t block_x, std::uint32_t block_y,
                          std::array<std::uint8_t, kTexelsPerBlock * kChannels>& out) {
            for (std::uint32_t y = 0; y < as::kBlockSize; ++y) {
                const std::uint32_t sy = std::min(block_y + y, level.height - 1);
                for (std::uint32_t x = 0; x < as::kBlockSize; ++x) {
                    const std::uint32_t sx = std::min(block_x + x, level.width - 1);
                    const std::size_t from =
                        ((static_cast<std::size_t>(sy) * level.width) + sx) * kChannels;
                    const std::size_t to =
                        ((static_cast<std::size_t>(y) * as::kBlockSize) + x) * kChannels;
                    std::memcpy(out.data() + to, level.texels.data() + from, kChannels);
                }
            }
        }

        /// Compresses one level to BC7 and appends it to the payload.
        void compress_level(const Level& level, const bc7enc_compress_block_params& params,
                            std::vector<std::byte>& payload) {
            const std::uint32_t across = (level.width + as::kBlockSize - 1) / as::kBlockSize;
            const std::uint32_t down = (level.height + as::kBlockSize - 1) / as::kBlockSize;

            std::array<std::uint8_t, kTexelsPerBlock * kChannels> block{};
            std::array<std::uint8_t, kBytesPerBlock> encoded{};

            for (std::uint32_t by = 0; by < down; ++by) {
                for (std::uint32_t bx = 0; bx < across; ++bx) {
                    gather_block(level, bx * as::kBlockSize, by * as::kBlockSize, block);
                    // The return value says whether the block held any alpha
                    // below 255. That is information, not an error, and this
                    // rule has no use for it.
                    (void)bc7enc_compress_block(encoded.data(), block.data(), &params);
                    const auto* first = reinterpret_cast<const std::byte*>(encoded.data());
                    payload.insert(payload.end(), first, first + encoded.size());
                }
            }
        }

        /// Appends one level to the payload with no compression.
        void copy_level(const Level& level, std::vector<std::byte>& payload) {
            const auto* first = reinterpret_cast<const std::byte*>(level.texels.data());
            payload.insert(payload.end(), first, first + level.texels.size());
        }

        [[nodiscard]] bool write_file(const std::filesystem::path& destination,
                                      const as::TextureHeader& header,
                                      const std::vector<std::byte>& payload) {
            std::ofstream file(destination, std::ios::binary | std::ios::trunc);
            if (!file) {
                ENGINE_LOG_ERROR("{}: could not open it for writing.", destination.string());
                return false;
            }
            file.write(reinterpret_cast<const char*>(&header), sizeof(header));
            file.write(reinterpret_cast<const char*>(payload.data()),
                       static_cast<std::streamsize>(payload.size()));
            if (!file) {
                ENGINE_LOG_ERROR("{}: the write failed part way through.", destination.string());
                return false;
            }
            return true;
        }

    } // namespace

    bool is_image_extension(const std::string& extension) {
        const std::string lower = lowered(extension);
        return lower == ".png" || lower == ".jpg" || lower == ".jpeg" || lower == ".tga" ||
               lower == ".bmp" || lower == ".psd";
    }

    as::ColorSpace guess_color_space(const std::filesystem::path& source) {
        const std::string name = lowered(source.stem().string());
        constexpr std::array<const char*, 8> kLinearNames{
            "normal",
            "roughness",
            "metallic",
            "metalness",
            "occlusion",
            "height",
            "mask",
            "orm",
        };
        for (const char* hint : kLinearNames) {
            if (name.find(hint) != std::string::npos) {
                return as::ColorSpace::Linear;
            }
        }
        return as::ColorSpace::Srgb;
    }

    bool cook_texture(const std::filesystem::path& source,
                      const std::filesystem::path& destination,
                      const as::TextureImport& settings) {
        StbImage image;
        int channels = 0;
        // Four channels always, whatever the file holds. A three-channel upload
        // is not a format every GPU has, and the fourth byte costs a quarter of
        // the memory only on the path that stays uncompressed.
        image.pixels = stbi_load(source.string().c_str(), &image.width, &image.height, &channels,
                                 static_cast<int>(kChannels));
        if (image.pixels == nullptr) {
            ENGINE_LOG_ERROR("{}: stb_image will not read it. {}", source.string(),
                             stbi_failure_reason());
            return false;
        }
        if (image.width <= 0 || image.height <= 0) {
            ENGINE_LOG_ERROR("{}: it holds no texels.", source.string());
            return false;
        }

        Level base;
        base.width = static_cast<std::uint32_t>(image.width);
        base.height = static_cast<std::uint32_t>(image.height);
        base.texels.assign(image.pixels,
                           image.pixels + (static_cast<std::size_t>(base.width) * base.height *
                                           kChannels));

        const std::uint32_t mip_count =
            settings.mips ? as::mip_count_for(base.width, base.height) : 1U;

        // BC7 needs at least one whole block. An image narrower or shorter than
        // a block would be all padding, so it stays uncompressed rather than
        // growing.
        const bool compress = settings.compress && base.width >= as::kBlockSize &&
                              base.height >= as::kBlockSize;
        const as::TextureFormat format =
            compress ? as::TextureFormat::BC7 : as::TextureFormat::RGBA8;

        bc7enc_compress_block_params params{};
        bc7enc_compress_block_params_init(&params);
        if (settings.color_space == as::ColorSpace::Linear) {
            // The perceptual default weights the error as an eye sees color.
            // A normal map or a roughness map holds numbers, not color, so it
            // wants every channel weighted the same.
            bc7enc_compress_block_params_init_linear_weights(&params);
        }
        if (compress) {
            bc7enc_compress_block_init();
        }

        std::vector<std::byte> payload;
        payload.reserve(
            as::chain_bytes(format, base.width, base.height, mip_count));

        Level level = std::move(base);
        for (std::uint32_t index = 0; index < mip_count; ++index) {
            if (index > 0) {
                level = halve(level, settings.color_space);
            }
            if (compress) {
                compress_level(level, params, payload);
            } else {
                copy_level(level, payload);
            }
        }

        as::TextureHeader header;
        header.format = static_cast<std::uint32_t>(format);
        header.color_space = static_cast<std::uint32_t>(settings.color_space);
        header.width = static_cast<std::uint32_t>(image.width);
        header.height = static_cast<std::uint32_t>(image.height);
        header.mip_count = mip_count;
        header.payload_size = static_cast<std::uint32_t>(payload.size());

        return write_file(destination, header, payload);
    }

} // namespace cooker
