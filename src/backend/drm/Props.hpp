#pragma once

#include <aquamarine/backend/DRM.hpp>

namespace Aquamarine {
    bool getDRMConnectorProps(int fd, uint32_t id, SDRMConnector::UDRMConnectorProps* out);
    bool getDRMConnectorColorspace(int fd, uint32_t id, SDRMConnector::UDRMConnectorColorspace* out, std::vector<IOutput::SColorspace>* supported);
    bool getDRMCRTCProps(int fd, uint32_t id, SDRMCRTC::UDRMCRTCProps* out);
    bool getDRMPlaneProps(int fd, uint32_t id, SDRMPlane::UDRMPlaneProps* out);
    struct SDRMPlaneColorRange {
        std::optional<uint64_t> full, limited, automatic;

        std::optional<uint64_t> select(eOutputColorRange range, bool preferFull) const;
    };

    SDRMPlaneColorRange getDRMPlaneColorRange(int fd, uint32_t plane, uint32_t prop);
    void                rememberDRMPlaneColorRange(const SDRMPlane& plane, const SDRMPlaneColorRange& range);
    SDRMPlaneColorRange rememberedDRMPlaneColorRange(const SDRMPlane& plane);
    void                forgetDRMPlaneColorRange(const SDRMPlane& plane);
    bool                getDRMProp(int fd, uint32_t obj, uint32_t prop, uint64_t* ret);
    void*               getDRMPropBlob(int fd, uint32_t obj, uint32_t prop, size_t* ret_len);
    char*               getDRMPropEnum(int fd, uint32_t obj, uint32_t prop_id);
    bool                introspectDRMPropRange(int fd, uint32_t prop_id, uint64_t* min, uint64_t* max);
};
