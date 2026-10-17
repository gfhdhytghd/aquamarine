#include "ColorPipeline.hpp"
#include "Props.hpp"
#include <cstring>
#include <aquamarine/output/Output.hpp>
#include "shared.hpp"
#include <set>

using namespace Aquamarine;

// Interpose only property discovery; these tests never access a DRM device.
static drmModePropertyRes     mockProperty      = {};
static bool                   propertyAvailable = true;
static int                    propertyReads = 0, propertyFrees = 0;
extern "C" drmModePropertyPtr drmModeGetProperty(int fd, uint32_t id) {
    ++propertyReads;
    return propertyAvailable ? &mockProperty : nullptr;
}
extern "C" void drmModeFreeProperty(drmModePropertyPtr prop) {
    ++propertyFrees;
}

int main() {
    int                         ret = 0;
    const std::vector<uint16_t> lut = {0, 10, 20, 65535, 65534, 65533};
    EXPECT(ColorPipeline::validLut(lut, 2), true);
    EXPECT(ColorPipeline::validLut(lut, 3), false);
    EXPECT(ColorPipeline::validLut({1, 2, 3, 4}, 1), false);
    EXPECT(ColorPipeline::validLut({}, 0), true);
    EXPECT(ColorPipeline::validLut({1, 2, 3}, 0), false);
    EXPECT(ColorPipeline::validLut({}, 2), true);
    const auto packed = ColorPipeline::packLut(lut);
    EXPECT(packed.size(), 2U);
    EXPECT(packed[0].green, 10);
    EXPECT(packed[1].red, 65535);
    EXPECT(packed[1].blue, 65533);
    EXPECT(packed[1].reserved, 0);

    // Model the blob ownership operations used by apply/rollback, including zero disable,
    // test-only/failure rollback, unchanged flips, retry reuse and final teardown.
    std::set<uint32_t> live    = {10, 11};
    auto               destroy = [&](uint32_t id) {
        if (id) {
            EXPECT(live.erase(id), 1U);
        }
    };
    uint32_t current = 10;
    ColorPipeline::finishBlob(current, 11, true, false, destroy);
    EXPECT(current, 10U);
    EXPECT(live.contains(10), true);
    EXPECT(live.contains(11), false);
    ColorPipeline::finishBlob(current, 0, false, true, destroy);
    EXPECT(current, 10U);
    live.insert(12);
    // A retry reuses candidate 12; only its final outcome performs cleanup.
    ColorPipeline::finishBlob(current, 12, true, true, destroy);
    EXPECT(current, 12U);
    EXPECT(live.contains(10), false);
    ColorPipeline::finishBlob(current, current, true, true, destroy);
    EXPECT(live.contains(12), true);
    ColorPipeline::finishBlob(current, 0, true, false, destroy);
    EXPECT(current, 12U);
    ColorPipeline::finishBlob(current, 0, true, true, destroy);
    EXPECT(current, 0U);
    EXPECT(live.empty(), true);
    live.insert(13);
    current = 13;
    ColorPipeline::finishBlob(current, 0, true, true, destroy);
    EXPECT(live.empty(), true);

    drm_mode_property_enum enums[3] = {};
    std::strcpy(enums[0].name, "Default");
    enums[0].value = 0;
    std::strcpy(enums[1].name, "BT2020_RGB");
    enums[1].value = 9;
    std::strcpy(enums[2].name, "driver_specific");
    enums[2].value           = uint64_t{1} << 40;
    mockProperty.flags       = DRM_MODE_PROP_ENUM;
    mockProperty.count_enums = 3;
    mockProperty.enums       = enums;
    SDRMConnector::UDRMConnectorColorspace colorspace;
    std::vector<IOutput::SColorspace>      supported;
    EXPECT(getDRMConnectorColorspace(-1, 1, &colorspace, &supported), true);
    EXPECT(supported.size(), 3U);
    EXPECT(supported[0].value, 0U);
    EXPECT(supported[0].name == "Default", true);
    EXPECT(supported[2].value, uint64_t{1} << 40);
    EXPECT(colorspace.values.BT2020_RGB, 9U);
    EXPECT(propertyFrees, 1);
    EXPECT(getDRMConnectorColorspace(-1, 0, &colorspace, &supported), false);
    EXPECT(supported.empty(), true);
    EXPECT(propertyReads, 1);
    mockProperty.flags = DRM_MODE_PROP_RANGE;
    EXPECT(getDRMConnectorColorspace(-1, 1, &colorspace, &supported), false);
    EXPECT(supported.empty(), true);
    EXPECT(propertyFrees, 2);
    propertyAvailable = false;
    EXPECT(getDRMConnectorColorspace(-1, 1, &colorspace, &supported), false);
    EXPECT(supported.empty(), true);
    EXPECT(propertyFrees, 2);

    COutputState state;
    state.setGammaLut(lut);
    state.setDeGammaLut(lut);
    const auto snapshot = state.snapshot();
    state.setGammaLut({}); // Gamma-control/CM share a slot, last write wins.
    state.consume(snapshot);
    EXPECT(state.state().gammaLut.empty(), true);
    EXPECT(!!(state.state().committed & COutputState::AQ_OUTPUT_STATE_GAMMA_LUT), true);
    EXPECT(!!(state.state().committed & COutputState::AQ_OUTPUT_STATE_DEGAMMA_LUT), false);
    EXPECT(snapshot.state().degammaLut == lut, true);
    state.rearm(snapshot);
    EXPECT(state.state().gammaLut.empty(), true);
    EXPECT(!!(state.state().committed & COutputState::AQ_OUTPUT_STATE_DEGAMMA_LUT), true);
    return ret;
}
