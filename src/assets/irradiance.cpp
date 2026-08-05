#include "assets/irradiance.h"

#include "core/log.h"

#include <cstring>

namespace engine::assets {

    bool read_irradiance(std::span<const std::byte> bytes, IrradianceSH& out,
                         std::string_view where) {
        if (bytes.size() < sizeof(IrradianceHeader)) {
            ENGINE_LOG_ERROR("{}: too short to be cooked irradiance. It holds {} bytes.", where,
                             bytes.size());
            return false;
        }

        // A copy, not a cast. The file may sit at any alignment in the caller's
        // buffer, and reading a struct through a misaligned pointer is undefined.
        IrradianceHeader header{};
        std::memcpy(&header, bytes.data(), sizeof(header));

        if (header.magic != kIrradianceMagic) {
            ENGINE_LOG_ERROR("{}: not cooked irradiance. Cook the content tree again.", where);
            return false;
        }
        if (header.version != kIrradianceVersion) {
            ENGINE_LOG_ERROR("{}: written by version {} and this build reads version {}. "
                             "Cook the content tree again.",
                             where, header.version, kIrradianceVersion);
            return false;
        }
        // The shape is fixed by the format rather than chosen by the writer, so
        // a file that disagrees was written by something else.
        if (header.coefficients != kIrradianceCoefficients ||
            header.channels != kIrradianceChannels) {
            ENGINE_LOG_ERROR("{}: it says {} coefficients of {} channels, and this build reads "
                             "{} of {}.",
                             where, header.coefficients, header.channels,
                             kIrradianceCoefficients, kIrradianceChannels);
            return false;
        }

        const std::size_t wanted = static_cast<std::size_t>(kIrradianceCoefficients) *
                                   kIrradianceChannels * sizeof(float);
        const std::size_t have = bytes.size() - sizeof(IrradianceHeader);
        if (have != wanted) {
            ENGINE_LOG_ERROR("{}: the coefficients need {} bytes and the file holds {}.", where,
                             wanted, have);
            return false;
        }

        std::memcpy(out.c.data(), bytes.data() + sizeof(IrradianceHeader), wanted);
        return true;
    }

} // namespace engine::assets
