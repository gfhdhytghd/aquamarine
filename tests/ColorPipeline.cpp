#include "ColorPipeline.hpp"
#include "Props.hpp"
#include <cstring>
#include <limits>
#include <aquamarine/backend/drm/Atomic.hpp>
#include <aquamarine/output/Output.hpp>
#include "shared.hpp"
#include <set>

using namespace Aquamarine;

// Interpose only property discovery; these tests never access a DRM device.
static drmModePropertyRes             mockProperty      = {};
static bool                           propertyAvailable = true;
static int                            propertyReads = 0, propertyFrees = 0;
static drmModeObjectProperties        mockObjectProperties      = {};
static bool                           objectPropertiesAvailable = false;

extern "C" drmModeObjectPropertiesPtr drmModeObjectGetProperties(int fd, uint32_t id, uint32_t type) {
    return objectPropertiesAvailable ? &mockObjectProperties : nullptr;
}
extern "C" void drmModeFreeObjectProperties(drmModeObjectPropertiesPtr props) {
    ;
}
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

    auto coefficients = Hyprutils::Math::Mat3x3::identity().getMatrix();
    coefficients[0]   = -1.5F;
    coefficients[1]   = 1.5F;
    coefficients[2]   = -0x1p-40F; // Truncates to zero.
    coefficients[3]   = std::nextafter(0x1p31F, 0.0F);
    const auto ctm    = ColorPipeline::encodeCTM(Hyprutils::Math::Mat3x3{coefficients});
    EXPECT(ctm.has_value(), true);
    if (ctm) {
        EXPECT(ctm->matrix[0], 0x8000000180000000ULL);
        EXPECT(ctm->matrix[1], 0x180000000ULL);
        EXPECT(ctm->matrix[2], 0U);
        EXPECT(ctm->matrix[3], 0x7FFFFF8000000000ULL);
    }
    for (const float invalid : {0x1p31F, -0x1p31F, 2147484160.0F, 0x1p32F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        coefficients[0] = invalid;
        EXPECT(ColorPipeline::encodeCTM(Hyprutils::Math::Mat3x3{coefficients}).has_value(), false);
    }

    using Hyprutils::Memory::makeShared;
    const auto connector                  = makeShared<SDRMConnector>();
    connector->crtc                       = makeShared<SDRMCRTC>();
    connector->atomic.propsCached         = true;
    connector->atomic.maxBpc              = 8;
    connector->crtc->atomic.ctmStateKnown = true;
    connector->crtc->atomic.ctm           = 10;
    connector->crtc->atomic.hdr           = 11;

    CDRMAtomicRequest request({});
    request.setConnector(connector);
    SDRMConnectorCommitData disabled = {};
    disabled.atomic.maxBpc           = connector->atomic.maxBpc; // The disabled request omitted the cap.
    disabled.test                    = true;
    request.apply(disabled);
    EXPECT(connector->atomic.propsCached, true);
    EXPECT(connector->crtc->atomic.ctmStateKnown, true);
    disabled.test = false;
    request.rollback(disabled);
    EXPECT(connector->atomic.propsCached, true);
    EXPECT(connector->crtc->atomic.ctmStateKnown, true);
    request.apply(disabled);
    EXPECT(connector->atomic.propsCached, false);
    EXPECT(connector->atomic.maxBpc, 8U);
    EXPECT(connector->crtc->atomic.ctmStateKnown, false);
    EXPECT(connector->crtc->atomic.ctm, 10U);
    EXPECT(connector->crtc->atomic.hdr, 11U);
    // The desired identity was consumed by the disabled commit. An enabling modeset must send it again.
    EXPECT(ColorPipeline::shouldSubmitCTM(true, true, false, connector->crtc->atomic.ctmStateKnown, connector->crtc->atomic.ctm, true), true);
    EXPECT(ColorPipeline::shouldSubmitCTM(true, true, false, true, true, true), true);
    EXPECT(ColorPipeline::shouldSubmitCTM(true, true, false, true, false, true), false);
    EXPECT(ColorPipeline::shouldSubmitCTM(true, false, true, true, true, true), true);

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

    propertyAvailable                = true;
    objectPropertiesAvailable        = true;
    uint32_t planeProp               = 1;
    uint64_t baseline                = 0;
    mockObjectProperties.count_props = 1;
    mockObjectProperties.props       = &planeProp;
    mockObjectProperties.prop_values = &baseline;

    std::strcpy(enums[0].name, "YCbCr limited range");
    enums[0].value = 0;
    std::strcpy(enums[1].name, "YCbCr full range");
    enums[1].value           = uint64_t{1} << 40;
    mockProperty.flags       = DRM_MODE_PROP_ENUM;
    mockProperty.count_enums = 2;

    const auto ranges = getDRMPlaneColorRange(-1, 2, planeProp);
    EXPECT(ranges.select(AQ_OUTPUT_COLOR_RANGE_FULL, false).value_or(99), uint64_t{1} << 40);
    EXPECT(ranges.select(AQ_OUTPUT_COLOR_RANGE_AUTO, false).value_or(99), 0U);
    EXPECT(ranges.select(AQ_OUTPUT_COLOR_RANGE_AUTO, true).value_or(99), uint64_t{1} << 40);
    mockProperty.count_enums = 1;
    const auto limitedOnly   = getDRMPlaneColorRange(-1, 2, planeProp);
    EXPECT(limitedOnly.select(AQ_OUTPUT_COLOR_RANGE_FULL, false).has_value(), false);
    EXPECT(limitedOnly.select(AQ_OUTPUT_COLOR_RANGE_LIMITED, false).value_or(99), 0U);
    EXPECT(limitedOnly.select(AQ_OUTPUT_COLOR_RANGE_AUTO, true).value_or(99), 0U);
    objectPropertiesAvailable = false;
    EXPECT(getDRMPlaneColorRange(-1, 2, planeProp).select(AQ_OUTPUT_COLOR_RANGE_LIMITED, false).has_value(), false);

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
