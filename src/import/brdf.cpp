#include "import/brdf.h"

#include "assets/texture.h"
#include "core/log.h"
#include "import/to_half.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <numbers>
#include <vector>

namespace as = engine::assets;

namespace engine::import {

    namespace {

        /// How many channels the cooked table carries. Two are used.
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

        /// The radical inverse of an index, which is the second Hammersley axis.
        [[nodiscard]] float radical_inverse(std::uint32_t bits) {
            constexpr std::uint32_t kHalfWord = 16U;
            constexpr std::uint32_t kByte = 8U;
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
            bits = ((bits & kLowBytes) << kByte) | ((bits & kHighBytes) >> kByte);

            constexpr float kInverseTwoPow32 = 2.3283064365386963e-10F;
            return static_cast<float>(bits) * kInverseTwoPow32;
        }

        /**
         * The Smith geometry term for image based lighting.
         *
         * The remapping of roughness differs from the one a punctual light
         * uses. Direct lighting takes `(roughness + 1) squared over 8`, and this
         * takes `roughness squared over 2`. Using the direct form here darkens
         * every rough reflection, and the two look similar enough that the
         * mistake survives a glance. See `mesh.frag` for the other one.
         */
        [[nodiscard]] float geometry_smith_ibl(float n_dot_v, float n_dot_l, float roughness) {
            // Alpha is roughness squared, and k is half of alpha. Squaring alpha
            // again here leaves k far too small at low roughness, which stops
            // the term shadowing anything at a grazing angle and sends the
            // integral past one. The test on energy is what catches it.
            const float alpha = roughness * roughness;
            const float k = alpha / 2.0F;
            const float view = n_dot_v / ((n_dot_v * (1.0F - k)) + k);
            const float light = n_dot_l / ((n_dot_l * (1.0F - k)) + k);
            return view * light;
        }

        /**
         * Integrates one entry of the table.
         *
         * The Fresnel term factors out of the specular integral, which is what
         * makes two numbers enough. Splitting `F` into the part that scales the
         * reflectance at normal incidence and the part that adds to it leaves a
         * scale and a bias that depend on nothing but the angle and the
         * roughness. A shader then rebuilds any material's answer from its own
         * `F0`.
         *
         * The normal is +Z here, and the viewer sits in the XZ plane, because
         * the lobe is round about the normal and nothing in the integral turns
         * with the surface.
         */
        void integrate_entry(float n_dot_v, float roughness, std::uint32_t samples, float& scale,
                             float& bias) {
            constexpr float kPi = std::numbers::pi_v<float>;

            const float view_x = std::sqrt(std::max(0.0F, 1.0F - (n_dot_v * n_dot_v)));
            const float view_z = n_dot_v;

            float sum_scale = 0.0F;
            float sum_bias = 0.0F;
            const float a = roughness * roughness;

            for (std::uint32_t i = 0; i < samples; ++i) {
                const float u1 = static_cast<float>(i) / static_cast<float>(samples);
                const float u2 = radical_inverse(i);

                // The GGX half vector, around a normal of +Z.
                const float phi = 2.0F * kPi * u1;
                const float cos_theta =
                    std::sqrt((1.0F - u2) / (1.0F + (((a * a) - 1.0F) * u2)));
                const float sin_theta =
                    std::sqrt(std::max(0.0F, 1.0F - (cos_theta * cos_theta)));

                // The y term of the half vector is worked out and dropped. The
                // viewer lies in the XZ plane, so nothing below reads it, and
                // the azimuth still matters because it decides half_x.
                const float half_x = sin_theta * std::cos(phi);
                const float half_z = cos_theta;

                const float v_dot_h = (view_x * half_x) + (view_z * half_z);

                // Reflect the viewer about the half vector.
                const float light_z = (2.0F * v_dot_h * half_z) - view_z;
                const float n_dot_l = light_z;
                if (n_dot_l <= 0.0F) {
                    continue;
                }

                const float n_dot_h = std::max(half_z, 0.0F);
                const float clamped_v_dot_h = std::max(v_dot_h, 0.0F);
                if (n_dot_h <= 0.0F || n_dot_v <= 0.0F) {
                    continue;
                }

                const float geometry = geometry_smith_ibl(n_dot_v, n_dot_l, roughness);
                // The probability of drawing this half vector cancels most of
                // the integrand, and this is what is left of it.
                const float visibility =
                    (geometry * clamped_v_dot_h) / (n_dot_h * n_dot_v);

                // Schlick's Fresnel, with the reflectance at normal incidence
                // pulled out. What is left multiplies the scale and the bias.
                const float fresnel =
                    std::pow(1.0F - clamped_v_dot_h, 5.0F); // NOLINT(*-magic-numbers)

                sum_scale += (1.0F - fresnel) * visibility;
                sum_bias += fresnel * visibility;
            }

            scale = sum_scale / static_cast<float>(samples);
            bias = sum_bias / static_cast<float>(samples);
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

    bool is_brdf_extension(const std::string& extension) {
        return lowered(extension) == ".brdf";
    }

    bool cook_brdf(const std::filesystem::path& destination, const as::BrdfImport& settings) {
        if (settings.size == 0 || settings.size > as::kMaxBrdfSize) {
            ENGINE_LOG_ERROR("The sidecar asks for a BRDF table of {} texels, and the range "
                             "is 1 to {}.",
                             settings.size, as::kMaxBrdfSize);
            return false;
        }
        // Zero rays would divide by nothing on every entry, and a number nobody
        // meant would look like a hung build rather than a mistake.
        if (settings.samples == 0 || settings.samples > as::kMaxBrdfSamples) {
            ENGINE_LOG_ERROR("The sidecar asks for {} rays for each BRDF entry, and the range "
                             "is 1 to {}.",
                             settings.samples, as::kMaxBrdfSamples);
            return false;
        }

        const std::uint32_t size = settings.size;
        std::vector<std::byte> payload(static_cast<std::size_t>(size) * size *
                                       static_cast<std::size_t>(kChannels) *
                                       sizeof(std::uint16_t));
        auto* texels = reinterpret_cast<std::uint16_t*>(payload.data());

        for (std::uint32_t y = 0; y < size; ++y) {
            // Row is roughness. The half texel offset puts the sample at the
            // texel center, so a shader reading with a linear filter lands on
            // the value this entry holds rather than between two of them.
            const float roughness =
                (static_cast<float>(y) + 0.5F) / static_cast<float>(size);

            for (std::uint32_t x = 0; x < size; ++x) {
                // Column is the cosine of the angle to the viewer. It never
                // reaches zero, because a viewer exactly edge on sees no
                // surface and the integral is not defined there.
                const float n_dot_v =
                    (static_cast<float>(x) + 0.5F) / static_cast<float>(size);

                float scale = 0.0F;
                float bias = 0.0F;
                integrate_entry(n_dot_v, roughness, settings.samples, scale, bias);

                const std::size_t at = (((static_cast<std::size_t>(y) * size) + x) *
                                        static_cast<std::size_t>(kChannels));
                texels[at] = to_half(scale);
                texels[at + 1] = to_half(bias);
                texels[at + 2] = to_half(0.0F);
                texels[at + 3] = to_half(1.0F);
            }
        }

        if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
            ENGINE_LOG_ERROR("The BRDF table is {} bytes, and a cooked texture records its "
                             "size in 32 bits.",
                             payload.size());
            return false;
        }

        as::TextureHeader header;
        header.format = static_cast<std::uint32_t>(as::TextureFormat::RGBA16F);
        // The table holds numbers rather than colour, so nothing converts.
        header.color_space = static_cast<std::uint32_t>(as::ColorSpace::Linear);
        header.width = size;
        header.height = size;
        // One level. A mip chain would average entries of different roughness
        // together, and a shader reads this at one level anyway.
        header.mip_count = 1;
        header.payload_size = static_cast<std::uint32_t>(payload.size());
        header.face_count = 1;

        return write_file(destination, header, payload);
    }

} // namespace engine::import
