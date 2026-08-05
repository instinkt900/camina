#include "environment.h"

#include "assets/irradiance.h"
#include "assets/reference.h"
#include "assets/texture.h"
#include "core/log.h"
#include "to_half.h"

#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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

        /// One cubemap face at one mip level, as linear floats.
        struct Face {
            std::vector<float> texels; ///< RGBA, row by row.
            std::uint32_t size = 0;    ///< Both axes, because a face is square.
        };

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

        /**
         * The radical inverse of an index, which is the second Hammersley axis.
         *
         * Reversing the bits of a counter gives a sequence that fills the unit
         * interval evenly at every prefix length. That is what makes a fixed
         * budget of rays cover the lobe rather than clumping, and it needs no
         * random number generator, so two cooks of one file agree exactly.
         */
        [[nodiscard]] float radical_inverse(std::uint32_t bits) {
            // The masks select every other bit, then every other pair, then
            // every other nibble, and so on. Each line swaps the two halves of
            // a group twice the width of the line before it, so five lines
            // reverse all 32 bits.
            constexpr std::uint32_t kHalfWord = 16U;
            constexpr std::uint32_t kOddBits = 0x55555555U;
            constexpr std::uint32_t kEvenBits = 0xAAAAAAAAU;
            constexpr std::uint32_t kLowPairs = 0x33333333U;
            constexpr std::uint32_t kHighPairs = 0xCCCCCCCCU;
            constexpr std::uint32_t kLowNibbles = 0x0F0F0F0FU;
            constexpr std::uint32_t kHighNibbles = 0xF0F0F0F0U;
            constexpr std::uint32_t kLowBytes = 0x00FF00FFU;
            constexpr std::uint32_t kHighBytes = 0xFF00FF00U;

            bits = (bits << kHalfWord) | (bits >> kHalfWord);
            bits = ((bits & kOddBits) << 1U) | ((bits & kEvenBits) >> 1U);
            bits = ((bits & kLowPairs) << 2U) | ((bits & kHighPairs) >> 2U);
            bits = ((bits & kLowNibbles) << 4U) | ((bits & kHighNibbles) >> 4U);
            constexpr std::uint32_t kByte = 8U;
            bits = ((bits & kLowBytes) << kByte) | ((bits & kHighBytes) >> kByte);

            // Two to the power of minus 32, so the reversed counter lands in
            // the unit interval.
            constexpr float kInverseTwoPow32 = 2.3283064365386963e-10F;
            return static_cast<float>(bits) * kInverseTwoPow32;
        }

        /// A unit vector, for the three-float directions this file passes about.
        struct Direction {
            float x = 0.0F;
            float y = 0.0F;
            float z = 0.0F;
        };

        [[nodiscard]] Direction normalized(Direction d) {
            const float length = std::sqrt((d.x * d.x) + (d.y * d.y) + (d.z * d.z));
            if (length <= 0.0F) {
                return { 0.0F, 1.0F, 0.0F };
            }
            return { d.x / length, d.y / length, d.z / length };
        }

        /**
         * One GGX half vector around @p normal, from a Hammersley pair.
         *
         * This is the importance sample of the Trowbridge-Reitz distribution.
         * Drawing towards where the lobe actually has weight is what keeps the
         * ray budget small: uniform sampling of the hemisphere would spend most
         * of its rays where the term is nearly zero.
         */
        [[nodiscard]] Direction importance_sample_ggx(float u1, float u2, float roughness,
                                                      Direction normal) {
            constexpr float kPi = std::numbers::pi_v<float>;
            const float a = roughness * roughness;

            const float phi = 2.0F * kPi * u1;
            const float cos_theta =
                std::sqrt((1.0F - u2) / (1.0F + (((a * a) - 1.0F) * u2)));
            const float sin_theta = std::sqrt(std::max(0.0F, 1.0F - (cos_theta * cos_theta)));

            const Direction half{ sin_theta * std::cos(phi), sin_theta * std::sin(phi),
                                  cos_theta };

            // Any frame around the normal will do, because the lobe is round
            // about it. The pick only has to avoid being parallel to it.
            const Direction up =
                std::abs(normal.z) < 0.999F ? Direction{ 0.0F, 0.0F, 1.0F }
                                            : Direction{ 1.0F, 0.0F, 0.0F };
            const Direction tangent = normalized({ (up.y * normal.z) - (up.z * normal.y),
                                                   (up.z * normal.x) - (up.x * normal.z),
                                                   (up.x * normal.y) - (up.y * normal.x) });
            const Direction bitangent{ (normal.y * tangent.z) - (normal.z * tangent.y),
                                       (normal.z * tangent.x) - (normal.x * tangent.z),
                                       (normal.x * tangent.y) - (normal.y * tangent.x) };

            return normalized({ (tangent.x * half.x) + (bitangent.x * half.y) +
                                    (normal.x * half.z),
                                (tangent.y * half.x) + (bitangent.y * half.y) +
                                    (normal.y * half.z),
                                (tangent.z * half.x) + (bitangent.z * half.y) +
                                    (normal.z * half.z) });
        }

        /**
         * Builds one mip level by averaging the GGX lobe for one roughness.
         *
         * The view direction is taken to be the normal, which is the standard
         * simplification: the true lobe stretches as a surface turns away, and
         * carrying that would need a second table. It costs a rough reflection
         * some of its stretch at a grazing angle and nothing else.
         *
         * The rays land on the panorama rather than on the level above. Filtering
         * an already filtered level would blur twice, and the panorama is the
         * only unfiltered thing here.
         */
        [[nodiscard]] Face prefilter_face(const Panorama& source, std::uint32_t face,
                                          std::uint32_t size, float roughness,
                                          std::uint32_t samples) {
            Face out;
            out.size = size;
            out.texels.resize(static_cast<std::size_t>(size) * size *
                              static_cast<std::size_t>(kChannels));

            for (std::uint32_t y = 0; y < size; ++y) {
                for (std::uint32_t x = 0; x < size; ++x) {
                    const float u =
                        ((static_cast<float>(x) + 0.5F) / static_cast<float>(size) * 2.0F) - 1.0F;
                    const float v =
                        ((static_cast<float>(y) + 0.5F) / static_cast<float>(size) * 2.0F) - 1.0F;

                    float dx = 0.0F;
                    float dy = 0.0F;
                    float dz = 0.0F;
                    face_direction(face, u, v, dx, dy, dz);
                    const Direction normal = normalized({ dx, dy, dz });

                    std::array<float, kChannels> total{};
                    float weight = 0.0F;

                    for (std::uint32_t i = 0; i < samples; ++i) {
                        const float u1 =
                            static_cast<float>(i) / static_cast<float>(samples);
                        const Direction half =
                            importance_sample_ggx(u1, radical_inverse(i), roughness, normal);

                        // Reflect the view about the half vector. With the view
                        // equal to the normal this is the mirror of the normal.
                        const float n_dot_h = (normal.x * half.x) + (normal.y * half.y) +
                                              (normal.z * half.z);
                        const Direction light{ (2.0F * n_dot_h * half.x) - normal.x,
                                               (2.0F * n_dot_h * half.y) - normal.y,
                                               (2.0F * n_dot_h * half.z) - normal.z };

                        const float n_dot_l = (normal.x * light.x) + (normal.y * light.y) +
                                              (normal.z * light.z);
                        if (n_dot_l <= 0.0F) {
                            continue;
                        }

                        std::array<float, kChannels> sampled{};
                        sample_panorama(source, light.x, light.y, light.z, sampled.data());
                        for (int c = 0; c < kChannels; ++c) {
                            total[static_cast<std::size_t>(c)] +=
                                sampled[static_cast<std::size_t>(c)] * n_dot_l;
                        }
                        weight += n_dot_l;
                    }

                    const std::size_t at = ((static_cast<std::size_t>(y) * size) + x) *
                                           static_cast<std::size_t>(kChannels);
                    for (int c = 0; c < kChannels; ++c) {
                        const auto channel = static_cast<std::size_t>(c);
                        // Every ray fell below the horizon, which a tiny level
                        // can manage. Black is wrong and the unfiltered texel is
                        // close enough at a size nothing looks at directly.
                        out.texels[at + channel] =
                            weight > 0.0F ? total[channel] / weight : 0.0F;
                    }
                }
            }
            return out;
        }

        /// The nine polynomials of the second order basis, without their constants.
        [[nodiscard]] std::array<float, as::kIrradianceCoefficients> sh_polynomials(
            Direction d) {
            // The zonal term of band two is 3z squared minus one.
            constexpr float kZonalScale = 3.0F;
            return { 1.0F,
                     d.y,
                     d.z,
                     d.x,
                     d.x * d.y,
                     d.y * d.z,
                     (kZonalScale * d.z * d.z) - 1.0F,
                     d.x * d.z,
                     (d.x * d.x) - (d.y * d.y) };
        }

        /**
         * Projects the panorama onto a second order spherical harmonic.
         *
         * Irradiance over a hemisphere is a very smooth function of the normal,
         * so nine coefficients carry it to within a percent or two. That is what
         * makes the diffuse half of image based lighting cost nine constants
         * rather than a texture read.
         *
         * The sum runs over the source rather than over the cubemap, because the
         * cubemap is already a resampling and the panorama is not.
         *
         * The convolution that turns radiance into irradiance is folded in here,
         * with the basis constants, so that a shader evaluates the plain
         * polynomials above and carries no table of its own. See
         * assets/irradiance.h for the exact sum it must write.
         */
        [[nodiscard]] as::IrradianceSH project_irradiance(const Panorama& source) {
            constexpr float kPi = std::numbers::pi_v<float>;

            // The normalization of each basis function, in the order above.
            constexpr float kBand0 = 0.282095F;
            constexpr float kBand1 = 0.488603F;
            constexpr float kBand2 = 1.092548F;
            constexpr float kBand2Zonal = 0.315392F;
            constexpr float kBand2Square = 0.546274F;
            constexpr std::array<float, as::kIrradianceCoefficients> kConstants{
                kBand0, kBand1, kBand1, kBand1, kBand2,
                kBand2, kBand2Zonal, kBand2, kBand2Square
            };

            // The per band convolution of a cosine lobe, from Ramamoorthi and
            // Hanrahan. Band 3 and above vanish, which is why nine is enough.
            constexpr float kCosineBand0 = kPi;
            constexpr float kCosineBand1 = 2.0F * kPi / 3.0F;
            constexpr float kCosineBand2 = kPi / 4.0F;
            constexpr std::array<float, as::kIrradianceCoefficients> kCosine{
                kCosineBand0, kCosineBand1, kCosineBand1, kCosineBand1, kCosineBand2,
                kCosineBand2, kCosineBand2, kCosineBand2, kCosineBand2
            };

            std::array<std::array<double, as::kIrradianceChannels>,
                       as::kIrradianceCoefficients>
                sums{};

            const auto width = static_cast<std::size_t>(source.width);
            const auto height = static_cast<std::size_t>(source.height);
            const float d_theta = kPi / static_cast<float>(height);
            const float d_phi = 2.0F * kPi / static_cast<float>(width);

            for (std::size_t y = 0; y < height; ++y) {
                // The row runs from the top, which is where acos starts, so
                // this matches what sample_panorama reads back.
                const float theta =
                    (static_cast<float>(y) + 0.5F) / static_cast<float>(height) * kPi;
                const float sin_theta = std::sin(theta);
                const float cos_theta = std::cos(theta);
                // A row near a pole covers almost no sphere, and this is what
                // stops it counting as much as a row at the equator.
                const float solid_angle = sin_theta * d_theta * d_phi;

                for (std::size_t x = 0; x < width; ++x) {
                    // The inverse of the mapping in sample_panorama, so the
                    // projection and the cubemap agree on which way is forward.
                    const float angle =
                        (((static_cast<float>(x) + 0.5F) / static_cast<float>(width)) - 0.5F) *
                        2.0F * kPi;
                    const Direction d{ std::sin(angle) * sin_theta, cos_theta,
                                       -std::cos(angle) * sin_theta };

                    const auto poly = sh_polynomials(d);
                    const std::size_t at = ((y * width) + x) * static_cast<std::size_t>(kChannels);

                    for (std::size_t i = 0; i < as::kIrradianceCoefficients; ++i) {
                        const double weight =
                            static_cast<double>(poly[i]) * static_cast<double>(solid_angle);
                        for (std::size_t c = 0; c < as::kIrradianceChannels; ++c) {
                            sums[i][c] += static_cast<double>(source.texels[at + c]) * weight;
                        }
                    }
                }
            }

            as::IrradianceSH out;
            for (std::size_t i = 0; i < as::kIrradianceCoefficients; ++i) {
                // The constant appears twice: once because the projection is
                // against the basis function, and once because the sum a shader
                // writes rebuilds it from the plain polynomial.
                const double scale = static_cast<double>(kCosine[i]) *
                                     static_cast<double>(kConstants[i]) *
                                     static_cast<double>(kConstants[i]);
                for (std::size_t c = 0; c < as::kIrradianceChannels; ++c) {
                    out.c[i][c] = static_cast<float>(sums[i][c] * scale);
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

        [[nodiscard]] bool write_irradiance(const std::filesystem::path& destination,
                                            const as::IrradianceSH& irradiance) {
            std::ofstream out(destination, std::ios::binary | std::ios::trunc);
            if (!out) {
                ENGINE_LOG_ERROR("{}: cannot open it to write.", destination.string());
                return false;
            }

            const as::IrradianceHeader header;
            out.write(reinterpret_cast<const char*>(&header), sizeof(header));
            // The coefficients are a plain array of floats, so they go as they
            // sit. assets::read_irradiance copies them back the same way.
            out.write(reinterpret_cast<const char*>(irradiance.c.data()),
                      static_cast<std::streamsize>(sizeof(irradiance.c)));
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
                          const std::filesystem::path& out_root,
                          const std::filesystem::path& relative, engine::Guid parent,
                          const as::EnvironmentImport& settings,
                          std::vector<as::ManifestOutput>& outputs) {
        // Both ends. A face of nothing writes an empty cubemap, and a face too
        // large overruns the 32-bit payload size a cooked texture records, which
        // would write a header the reader then refuses.
        if (settings.face_size == 0 || settings.face_size > as::kMaxFaceSize) {
            ENGINE_LOG_ERROR("{}: the sidecar asks for a face of {} texels, and a face is "
                             "from 1 to {}.",
                             source.string(), settings.face_size, as::kMaxFaceSize);
            return false;
        }

        // Zero rays would divide by nothing on every prefiltered texel, and a
        // number nobody meant would look like a hung build rather than a
        // mistake in a sidecar.
        if (settings.specular_samples == 0 ||
            settings.specular_samples > as::kMaxSpecularSamples) {
            ENGINE_LOG_ERROR("{}: the sidecar asks for {} rays for each prefiltered texel, "
                             "and the range is 1 to {}.",
                             source.string(), settings.specular_samples,
                             as::kMaxSpecularSamples);
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
            for (std::uint32_t index = 0; index < mip_count; ++index) {
                const std::uint32_t level_size = as::mip_extent(size, index);

                // Level 0 is the environment as it is. A mirror reflects it,
                // and importance sampling a lobe of no width would spend a
                // ray budget to arrive back at the same texel.
                if (index == 0) {
                    append_level(project_face(panorama, face, level_size), payload);
                    continue;
                }

                // Roughness runs from just above zero to one across the rest of
                // the chain, so a shader picks a level from the roughness a
                // material named.
                const float roughness = static_cast<float>(index) /
                                        static_cast<float>(mip_count - 1);
                append_level(prefilter_face(panorama, face, level_size, roughness,
                                            settings.specular_samples),
                             payload);
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

        // The cubemap keeps the identity of the source, so a scene that names
        // this environment needs no change and an older scene still resolves.
        const std::filesystem::path cubemap_name =
            std::filesystem::path(relative).concat(as::kTextureExtension);
        const std::filesystem::path cubemap = out_root / cubemap_name;
        std::error_code error;
        std::filesystem::create_directories(cubemap.parent_path(), error);
        if (!write_file(cubemap, header, payload)) {
            return false;
        }
        outputs.push_back(as::ManifestOutput{ .cooked = as::manifest_path(cubemap_name),
                                              .guid = parent });

        // The irradiance derives its identity, the way a mesh inside a glTF
        // does, so a caller that holds the environment can work out both parts
        // without the manifest telling it which is which.
        const std::filesystem::path irradiance_name =
            std::filesystem::path(relative).concat(as::kIrradianceExtension);
        if (!write_irradiance(out_root / irradiance_name, project_irradiance(panorama))) {
            return false;
        }
        outputs.push_back(as::ManifestOutput{
            .cooked = as::manifest_path(irradiance_name),
            .guid = engine::Guid::derive(parent, as::kIrradiancePartKind, 0) });

        return true;
    }

} // namespace cooker
