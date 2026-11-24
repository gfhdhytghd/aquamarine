#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <hyprutils/math/Mat3x3.hpp>
#include <vector>
#include <drm_mode.h>

namespace Aquamarine::ColorPipeline {
    inline std::optional<drm_color_ctm> encodeCTM(const Hyprutils::Math::Mat3x3& matrix) {
        drm_color_ctm result       = {};
        const auto    coefficients = matrix.getMatrix();
        for (size_t i = 0; i < coefficients.size(); ++i) {
            const double value     = coefficients[i];
            const double magnitude = std::abs(value);
            if (!std::isfinite(magnitude) || magnitude >= 0x1p31)
                return std::nullopt;
            const auto fixed = static_cast<uint64_t>(magnitude * 0x1p32);
            result.matrix[i] = value < 0 && fixed ? fixed | (uint64_t{1} << 63) : fixed;
        }
        return result;
    }

    inline bool shouldSubmitCTM(bool supported, bool modeset, bool dirty, bool known, bool hasBlob, bool identity) {
        return dirty || (modeset && (!identity || (supported && (!known || hasBlob))));
    }

    // An empty LUT disables the stage, which is valid even where the stage doesn't exist.
    inline bool validLut(const std::vector<uint16_t>& lut, size_t size) {
        return lut.empty() || (size > 0 && lut.size() % 3 == 0 && lut.size() / 3 == size);
    }

    inline std::vector<drm_color_lut> packLut(const std::vector<uint16_t>& lut) {
        std::vector<drm_color_lut> packed(lut.size() / 3);
        for (size_t i = 0; i < packed.size(); ++i)
            packed[i] = {.red = lut[i * 3], .green = lut[i * 3 + 1], .blue = lut[i * 3 + 2], .reserved = 0};
        return packed;
    }

    // Untouched properties keep their owned blobs. Empty updates replace them with zero.
    template <typename F>
    void finishBlob(uint32_t& current, uint32_t next, bool changed, bool accepted, F destroy) {
        if (!changed || current == next)
            return;
        if (accepted) {
            destroy(current);
            current = next;
        } else
            destroy(next);
    }
}
