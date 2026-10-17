#pragma once

#include <cstdint>
#include <vector>
#include <drm_mode.h>

namespace Aquamarine::ColorPipeline {
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
