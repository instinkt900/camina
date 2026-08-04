#include "environment.h"

#include "assets/texture.h"
#include "core/log.h"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <fstream>
#include <numbers>
#include <vector>

namespace as = engine::assets;

namespace cooker {

    namespace {

        /// How many channels the float image and every face carry.
        constexpr int kChannels = 4;

        /// A lowercase copy, so an extension compares in any letter case.
        [[nodiscard]] std::string lowered(const std::string& text) {
            std::string out;
            out.reserve(text.size());
            for (const char c : text) {
                out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
            }
            return out;
        }

        /// An equirectangular source image, as linear floats.
        struct Panorama {
            std::vector<float> texels; ///< RGBA, row by row from the top.
            int width = 0;
            int height = 0;
        };

        /// How many texels one 2 by 2 box filter averages.
        constexpr std::uint32_t kBoxTexels = 4;

        /// One cubemap face at one mip level, as linear floats.
        struct Face {
            std::vector<float> texels; ///< RGBA, row by row.
            std::uint32_t size = 0;    ///< Both axes, because a face is square.
        };

        /// Where each part of a 32-bit float sits, and what a half float holds.
        constexpr std::uint32_t kFloatMantissaBits = 23;
        constexpr std::uint32_t kFloatExponentMask = 0xFFU;
        constexpr std::uint32_t kFloatMantissaMask = 0x7FFFFFU;
        constexpr std::uint32_t kFloatImplicitOne = 0x800000U;
        constexpr std::int32_t kFloatExponentBias = 127;
        constexpr std::uint32_t kHalfMantissaBits = 10;
        constexpr std::int32_t kHalfExponentBias = 15;
        constexpr std::int32_t kHalfExponentMax = 0x1F;
        /// The sign bit of a half, reached by shifting a float down.
        constexpr std::uint32_t kHalfSignShift = 16;
        constexpr std::uint32_t kHalfSignMask = 0x8000U;
        /// The largest finite half float: exponent 30, every mantissa bit set.
        constexpr std::uint32_t kHalfLargestFinite = 0x7BFFU;
        /// Below this the value cannot be a denormal half either.
        constexpr std::int32_t kHalfDenormalFloor = -10;

        /**
         * Turns a float into a 16-bit half float.
         *
         * The cooked payload is half, because a sky needs the range of a float
         * and not the precision of one. This handles the three cases that a
         * naive shift gets wrong: a value too large for half, which clamps to
         * the largest finite half rather than becoming infinity, a value too
         * small, which flushes to zero rather than wrapping, and a denormal
         * result, which needs the mantissa shifted by hand.
         */
        [[nodiscard]] std::uint16_t to_half(float value) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));

            const std::uint32_t sign = (bits >> kHalfSignShift) & kHalfSignMask;
            const std::int32_t exponent =
                static_cast<std::int32_t>((bits >> kFloatMantissaBits) & kFloatExponentMask) -
                kFloatExponentBias + kHalfExponentBias;
            std::uint32_t mantissa = bits & kFloatMantissaMask;

            if (exponent >= kHalfExponentMax) {
                // Too large for half, or already an infinity or a NaN. Clamp to
                // the largest finite half. An infinity in an environment map
                // would poison every average the mip chain takes, and a NaN
                // would spread over the whole level.
                return static_cast<std::uint16_t>(sign | kHalfLargestFinite);
            }
            if (exponent <= 0) {
                // A denormal half, or a value too small to represent at all.
                if (exponent < kHalfDenormalFloor) {
                    return static_cast<std::uint16_t>(sign);
                }
                mantissa |= kFloatImplicitOne;
                const auto shift = static_cast<std::uint32_t>(
                    static_cast<std::int32_t>(kFloatMantissaBits - kHalfMantissaBits) + 1 -
                    exponent);
                return static_cast<std::uint16_t>(sign | (mantissa >> shift));
            }
            return static_cast<std::uint16_t>(
                sign | (static_cast<std::uint32_t>(exponent) << kHalfMantissaBits) |
                (mantissa >> (kFloatMantissaBits - kHalfMantissaBits)));
        }

        /// Reads one texel of the panorama, clamped at the poles and wrapped in x.
        [[nodiscard]] const float* texel_at(const Panorama& source, int x, int y) {
            // Wrapping x is what makes the seam behind the camera continuous.
            // Clamping y is right, because there is nothing above the top row.
            const int wrapped = ((x % source.width) + source.width) % source.width;
            const int clamped = std::clamp(y, 0, source.height - 1);
            const std::size_t at = ((static_cast<std::size_t>(clamped) *
                                     static_cast<std::size_t>(source.width)) +
                                    static_cast<std::size_t>(wrapped)) *
                                   static_cast<std::size_t>(kChannels);
            return source.texels.data() + at;
        }

        /**
         * Samples the panorama along a direction, with bilinear filtering.
         *
         * The direction to texture coordinate mapping decides which way the
         * environment faces, and getting it wrong turns the world inside out in
         * a way that is hard to see in a still picture. The sandbox environment
         * carries four differently colored quadrants for exactly that reason.
         */
        void sample_panorama(const Panorama& source, float dx, float dy, float dz, float* out) {
            const float length = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
            const float nx = dx / length;
            const float ny = dy / length;
            const float nz = dz / length;

            constexpr float kPi = std::numbers::pi_v<float>;
            // atan2(x, -z) puts u = 0.5 straight down −Z, which is the forward
            // direction DESIGN.md section 3 names. So the middle of the source
            // image is what the camera sees at rest.
            const float u = 0.5F + (std::atan2(nx, -nz) / (2.0F * kPi));
            // acos runs from 0 at the top to pi at the bottom, which matches an
            // image whose first row is the top.
            const float v = std::acos(std::clamp(ny, -1.0F, 1.0F)) / kPi;

            const float fx = (u * static_cast<float>(source.width)) - 0.5F;
            const float fy = (v * static_cast<float>(source.height)) - 0.5F;
            const int x0 = static_cast<int>(std::floor(fx));
            const int y0 = static_cast<int>(std::floor(fy));
            const float tx = fx - static_cast<float>(x0);
            const float ty = fy - static_cast<float>(y0);

            const float* t00 = texel_at(source, x0, y0);
            const float* t10 = texel_at(source, x0 + 1, y0);
            const float* t01 = texel_at(source, x0, y0 + 1);
            const float* t11 = texel_at(source, x0 + 1, y0 + 1);

            for (int c = 0; c < kChannels; ++c) {
                const float top = (t00[c] * (1.0F - tx)) + (t10[c] * tx);
                const float bottom = (t01[c] * (1.0F - tx)) + (t11[c] * tx);
                out[c] = (top * (1.0F - ty)) + (bottom * ty);
            }
        }

        /**
         * The direction one texel of one cubemap face points.
         *
         * This is the Vulkan face order and orientation, which is also the D3D
         * one: +X, −X, +Y, −Y, +Z, −Z, with u running right and v running down
         * inside each face. A sign wrong here mirrors the environment, and a
         * pair swapped rotates it, and neither one fails.
         *
         * @param face Which face, from 0 to 5.
         * @param u Across the face, from −1 to 1.
         * @param v Down the face, from −1 to 1.
         */
        void face_direction(std::uint32_t face, float u, float v, float& dx, float& dy,
                            float& dz) {
            switch (face) {
            case 0: // +X
                dx = 1.0F;
                dy = -v;
                dz = -u;
                break;
            case 1: // -X
                dx = -1.0F;
                dy = -v;
                dz = u;
                break;
            case 2: // +Y
                dx = u;
                dy = 1.0F;
                dz = v;
                break;
            case 3: // -Y
                dx = u;
                dy = -1.0F;
                dz = -v;
                break;
            case 4: // +Z
                dx = u;
                dy = -v;
                dz = 1.0F;
                break;
            default: // -Z
                dx = -u;
                dy = -v;
                dz = -1.0F;
                break;
            }
        }

        /// Projects the panorama onto one cubemap face at full size.
        [[nodiscard]] Face project_face(const Panorama& source, std::uint32_t face,
                                        std::uint32_t size) {
            Face out;
            out.size = size;
            out.texels.resize(static_cast<std::size_t>(size) * size *
                              static_cast<std::size_t>(kChannels));

            for (std::uint32_t y = 0; y < size; ++y) {
                for (std::uint32_t x = 0; x < size; ++x) {
                    // The half texel offset puts the sample at the texel center,
                    // so the six faces meet without a seam of doubled texels.
                    const float u =
                        ((static_cast<float>(x) + 0.5F) / static_cast<float>(size) * 2.0F) - 1.0F;
                    const float v =
                        ((static_cast<float>(y) + 0.5F) / static_cast<float>(size) * 2.0F) - 1.0F;

                    float dx = 0.0F;
                    float dy = 0.0F;
                    float dz = 0.0F;
                    face_direction(face, u, v, dx, dy, dz);

                    const std::size_t at =
                        ((static_cast<std::size_t>(y) * size) + x) *
                        static_cast<std::size_t>(kChannels);
                    sample_panorama(source, dx, dy, dz, out.texels.data() + at);
                }
            }
            return out;
        }

        /// Halves a face by averaging each 2 by 2 group, in linear light.
        [[nodiscard]] Face halve(const Face& source) {
            Face out;
            out.size = std::max(1U, source.size / 2);
            out.texels.resize(static_cast<std::size_t>(out.size) * out.size *
                              static_cast<std::size_t>(kChannels));

            for (std::uint32_t y = 0; y < out.size; ++y) {
                for (std::uint32_t x = 0; x < out.size; ++x) {
                    for (int c = 0; c < kChannels; ++c) {
                        float total = 0.0F;
                        for (std::uint32_t dy = 0; dy < 2; ++dy) {
                            for (std::uint32_t dx = 0; dx < 2; ++dx) {
                                const std::uint32_t sx = std::min((x * 2) + dx, source.size - 1);
                                const std::uint32_t sy = std::min((y * 2) + dy, source.size - 1);
                                const std::size_t at =
                                    (((static_cast<std::size_t>(sy) * source.size) + sx) *
                                     static_cast<std::size_t>(kChannels)) +
                                    static_cast<std::size_t>(c);
                                total += source.texels[at];
                            }
                        }
                        const std::size_t at =
                            (((static_cast<std::size_t>(y) * out.size) + x) *
                             static_cast<std::size_t>(kChannels)) +
                            static_cast<std::size_t>(c);
                        out.texels[at] = total / static_cast<float>(kBoxTexels);
                    }
                }
            }
            return out;
        }

        /// Appends one face level to the payload, as half floats.
        void append_level(const Face& level, std::vector<std::byte>& payload) {
            const std::size_t at = payload.size();
            payload.resize(at + (level.texels.size() * sizeof(std::uint16_t)));
            auto* out = reinterpret_cast<std::uint16_t*>(payload.data() + at);
            for (std::size_t i = 0; i < level.texels.size(); ++i) {
                out[i] = to_half(level.texels[i]);
            }
        }

        [[nodiscard]] bool write_file(const std::filesystem::path& destination,
                                      const as::TextureHeader& header,
                                      const std::vector<std::byte>& payload) {
            std::ofstream out(destination, std::ios::binary | std::ios::trunc);
            if (!out) {
                ENGINE_LOG_ERROR("{}: cannot open it to write.", destination.string());
                return false;
            }
            out.write(reinterpret_cast<const char*>(&header), sizeof(header));
            out.write(reinterpret_cast<const char*>(payload.data()),
                      static_cast<std::streamsize>(payload.size()));
            if (!out) {
                ENGINE_LOG_ERROR("{}: the write failed part way.", destination.string());
                return false;
            }
            return true;
        }

    } // namespace

    bool is_environment_extension(const std::string& extension) {
        return lowered(extension) == ".hdr";
    }

    bool cook_environment(const std::filesystem::path& source,
                          const std::filesystem::path& destination,
                          const as::EnvironmentImport& settings) {
        // Both ends. A face of nothing writes an empty cubemap, and a face too
        // large overruns the 32-bit payload size a cooked texture records, which
        // would write a header the reader then refuses.
        if (settings.face_size == 0 || settings.face_size > as::kMaxFaceSize) {
            ENGINE_LOG_ERROR("{}: the sidecar asks for a face of {} texels, and a face is "
                             "from 1 to {}.",
                             source.string(), settings.face_size, as::kMaxFaceSize);
            return false;
        }

        Panorama panorama;
        int channels = 0;
        float* pixels = stbi_loadf(source.string().c_str(), &panorama.width, &panorama.height,
                                   &channels, kChannels);
        if (pixels == nullptr) {
            ENGINE_LOG_ERROR("{}: stb_image will not read it as HDR. {}", source.string(),
                             stbi_failure_reason());
            return false;
        }

        const std::size_t count = static_cast<std::size_t>(panorama.width) *
                                  static_cast<std::size_t>(panorama.height) *
                                  static_cast<std::size_t>(kChannels);
        panorama.texels.assign(pixels, std::next(pixels, static_cast<std::ptrdiff_t>(count)));
        stbi_image_free(pixels);

        // An equirectangular image wraps the sphere once across and half way
        // down, so it is twice as wide as it is tall. A different ratio means
        // the file is a photograph rather than a panorama, and projecting it
        // would give a plausible looking cubemap of the wrong thing.
        if (panorama.width != panorama.height * 2) {
            ENGINE_LOG_WARN("{}: an equirectangular image is twice as wide as it is tall, and "
                            "this one is {} by {}. Cooking it anyway.",
                            source.string(), panorama.width, panorama.height);
        }

        const std::uint32_t size = settings.face_size;
        const std::uint32_t mip_count = as::mip_count_for(size, size);

        std::vector<std::byte> payload;
        payload.reserve(static_cast<std::size_t>(
            as::chain_bytes(as::TextureFormat::RGBA16F, size, size, mip_count) *
            as::kCubeFaceCount));

        // Face by face, and the whole chain of each before the next one starts.
        // That is the order assets::read_texture describes and the order the
        // device copies in.
        for (std::uint32_t face = 0; face < as::kCubeFaceCount; ++face) {
            Face level = project_face(panorama, face, size);
            for (std::uint32_t index = 0; index < mip_count; ++index) {
                if (index > 0) {
                    level = halve(level);
                }
                append_level(level, payload);
            }
        }

        // kMaxFaceSize keeps this inside a uint32, so this cannot fire today.
        // It is here because the bound and this field are two separate facts,
        // and a later change to one of them must not silently truncate.
        if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
            ENGINE_LOG_ERROR("{}: the cubemap is {} bytes, and a cooked texture records its "
                             "size in 32 bits.",
                             source.string(), payload.size());
            return false;
        }

        as::TextureHeader header;
        header.format = static_cast<std::uint32_t>(as::TextureFormat::RGBA16F);
        // An HDR file is linear by definition, so there is nothing to guess.
        header.color_space = static_cast<std::uint32_t>(as::ColorSpace::Linear);
        header.width = size;
        header.height = size;
        header.mip_count = mip_count;
        header.payload_size = static_cast<std::uint32_t>(payload.size());
        header.face_count = as::kCubeFaceCount;

        return write_file(destination, header, payload);
    }

} // namespace cooker
