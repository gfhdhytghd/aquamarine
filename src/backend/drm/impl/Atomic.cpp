#include <aquamarine/backend/drm/Atomic.hpp>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <sys/mman.h>
#include <sstream>
#include <optional>
#include "Shared.hpp"
#include "../ColorPipeline.hpp"
#include "../Props.hpp"
#include <hyprutils/utils/ScopeGuard.hpp>
#include "aquamarine/output/Output.hpp"

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;
using namespace Hyprutils::Utils;
#define SP CSharedPointer

// CTA-861.3 EOTF value, mirrors HDMI_EOTF_TRADITIONAL_GAMMA_SDR from the kernel's linux/hdmi.h (not exposed in uapi)
static constexpr uint8_t HDMI_EOTF_TRADITIONAL_GAMMA_SDR = 0;

Aquamarine::CDRMAtomicRequest::CDRMAtomicRequest(Hyprutils::Memory::CWeakPointer<CDRMBackend> backend_) : backend(backend_), req(drmModeAtomicAlloc()) {
    if (!req)
        failed = true;
}

Aquamarine::CDRMAtomicRequest::~CDRMAtomicRequest() {
    if (req)
        drmModeAtomicFree(req);
    for (const auto& blob : borrowedModeBlobs) {
        destroyBlob(blob);
    }
}

// Restates every other enabled head at its current mode, so the kernel re-runs
// its global modeset check across all of them instead of only ours.
//
// Drivers that allocate display bandwidth or head resources globally (nvidia,
// and i915 via intel_bw_atomic_check) can reject a modeset that touches a single
// connector while the other heads stay pinned in their existing configuration,
// even when the very same end state commits fine if every head is modeset at
// once. That is how a 4K@240 DSC mode ends up EINVAL next to two 1080p heads
// while weston, which configures all outputs in one go, gets it. (see #7912)
//
// A fresh blob is required: handing the kernel the same blob id it already holds
// leaves mode_changed clear, so the driver never redoes the allocation.
// Returns false if there was nothing to restate.
bool Aquamarine::CDRMAtomicRequest::restateConnectors(SP<SDRMConnector> self) {
    if (failed)
        return false;

    bool any = false;

    for (const auto& c : backend->connectors) {
        if (c == self || !c->crtc || !c->output)
            continue;

        if (!c->output->enabledState)
            continue;

        drmModeModeInfo* current = c->getCurrentMode();
        if (!current)
            continue;

        // getCurrentMode hands back a malloc'd copy. Take it by value and release
        // it right away, so the early exits below don't each need their own free.
        const drmModeModeInfo mode = *current;
        free(current); // I know this and other will fail lint

        uint32_t blob = 0;
        if (drmModeCreatePropertyBlob(backend->gpu->fd, &mode, sizeof(mode), &blob)) {
            backend->log(AQ_LOG_ERROR, std::format("atomic drm request: failed to blob mode for {} while restating heads", c->szName));
            continue;
        }

        borrowedModeBlobs.emplace_back(blob);

        TRACE(backend->log(AQ_LOG_TRACE, std::format("atomic drm request: restating head {} with blob {}", c->szName, blob)));

        add(c->crtc->id, c->crtc->props.values.mode_id, blob);
        add(c->crtc->id, c->crtc->props.values.active, 1);
        add(c->id, c->props.values.crtc_id, c->crtc->id);

        // A head that has not scanned out yet has no buffer to restate; its
        // existing plane state carries over untouched, which is what we want.
        if (c->crtc->primary && c->crtc->primary->front)
            planePropsInternal(c->crtc->primary, c->crtc->primary->front, c->crtc->id, {}, c->atomic.colorRange, false);

        any = true;
    }

    return any && !failed;
}

void Aquamarine::CDRMAtomicRequest::add(uint32_t id, uint32_t prop, uint64_t val) {
    if (failed)
        return;

    TRACE(backend->log(AQ_LOG_TRACE, std::format("atomic drm request: adding id {} prop {} with value {}", id, prop, val)));

    if (id == 0 || prop == 0) {
        backend->log(AQ_LOG_ERROR, "atomic drm request: failed to add prop: id / prop == 0");
        return;
    }

    if (drmModeAtomicAddProperty(req, id, prop, val) < 0) {
        backend->log(AQ_LOG_ERROR, "atomic drm request: failed to add prop");
        failed = true;
    }
}

bool Aquamarine::CDRMAtomicRequest::addRaw(uint32_t id, uint32_t prop, uint64_t val) noexcept {
    if (!req || !id || !prop)
        return false;

    return drmModeAtomicAddProperty(req, id, prop, val) >= 0;
}

CDRMAtomicRequest::SAsyncResult Aquamarine::CDRMAtomicRequest::submitAsync(int drmFD, uint32_t flags, uint64_t commitID) noexcept {
    if (!req || failed || drmFD < 0 || !commitID)
        return {.submitted = false, .error = EINVAL};

    const auto RET = drmModeAtomicCommit(drmFD, req, flags, rc<void*>(commitID));
    if (RET)
        return {.submitted = false, .error = RET == -1 ? errno : -RET};

    return {.submitted = true, .error = 0};
}

void Aquamarine::CDRMAtomicRequest::planeProps(Hyprutils::Memory::CSharedPointer<SDRMPlane> plane, Hyprutils::Memory::CSharedPointer<CDRMFB> fb, uint32_t crtc,
                                               Hyprutils::Math::Vector2D pos, eOutputColorRange colorRange) {
    planePropsInternal(plane, fb, crtc, pos, colorRange, true);
}

void Aquamarine::CDRMAtomicRequest::planePropsInternal(const SP<SDRMPlane>& plane, const SP<CDRMFB>& fb, uint32_t crtc, Vector2D pos, eOutputColorRange colorRange,
                                                       bool submitRange) {
    if (failed)
        return;

    if (!fb || !crtc) {
        // Disable the plane
        TRACE(backend->log(AQ_LOG_TRACE, std::format("atomic planeProps: disabling plane {}", plane->id)));
        add(plane->id, plane->props.values.fb_id, 0);
        add(plane->id, plane->props.values.crtc_id, 0);
        add(plane->id, plane->props.values.crtc_x, (uint64_t)(int64_t)pos.x);
        add(plane->id, plane->props.values.crtc_y, (uint64_t)(int64_t)pos.y);
        return;
    }

    TRACE(backend->log(AQ_LOG_TRACE,
                       std::format("atomic planeProps: prop blobs: src_x {}, src_y {}, src_w {}, src_h {}, crtc_w {}, crtc_h {}, fb_id {}, crtc_id {}", plane->props.values.src_x,
                                   plane->props.values.src_y, plane->props.values.src_w, plane->props.values.src_h, plane->props.values.crtc_w, plane->props.values.crtc_h,
                                   plane->props.values.fb_id, plane->props.values.crtc_id)));

    // src_ are 16.16 fixed point (lol)
    add(plane->id, plane->props.values.src_x, 0);
    add(plane->id, plane->props.values.src_y, 0);
    add(plane->id, plane->props.values.src_w, ((uint64_t)fb->buffer->size.x) << 16);
    add(plane->id, plane->props.values.src_h, ((uint64_t)fb->buffer->size.y) << 16);
    add(plane->id, plane->props.values.crtc_w, (uint32_t)fb->buffer->size.x);
    add(plane->id, plane->props.values.crtc_h, (uint32_t)fb->buffer->size.y);
    add(plane->id, plane->props.values.fb_id, fb->id);
    add(plane->id, plane->props.values.crtc_id, crtc);

    if (plane->props.values.color_range && submitRange) {
        const auto range = rememberedDRMPlaneColorRange(*plane);
        // NVIDIA uses full range by default in Aquamarine when the enum is available.
        const bool preferFull = backend->gpuDriver() == AQ_BACKEND_GPU_DRIVER_NVIDIA;
        const auto value      = range.select(colorRange, preferFull);
        if (value)
            add(plane->id, plane->props.values.color_range, *value);
        else if (colorRange != AQ_OUTPUT_COLOR_RANGE_AUTO) {
            backend->log(AQ_LOG_ERROR, "atomic drm: requested plane color range is unsupported");
            failed = true;
        }
    }

    planePropsPos(plane, pos);
}

void Aquamarine::CDRMAtomicRequest::planePropsPos(Hyprutils::Memory::CSharedPointer<SDRMPlane> plane, Hyprutils::Math::Vector2D pos) {

    if (failed)
        return;

    TRACE(backend->log(AQ_LOG_TRACE, std::format("atomic planeProps: pos blobs: crtc_x {}, crtc_y {}", plane->props.values.crtc_x, plane->props.values.crtc_y)));

    add(plane->id, plane->props.values.crtc_x, (uint64_t)(int64_t)pos.x);
    add(plane->id, plane->props.values.crtc_y, (uint64_t)(int64_t)pos.y);
}

void Aquamarine::CDRMAtomicRequest::setConnector(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector) {
    conn = connector;
}

void Aquamarine::CDRMAtomicRequest::addConnector(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data) {
    addConnectorInternal(connector, data, false);
}

void Aquamarine::CDRMAtomicRequest::addConnectorInternal(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data, bool omitMaxBpc) {
    conn = connector;
    if (failed)
        return;

    data.atomic.blobbed = false;
    const auto& STATE   = data.outputState;
    const bool  enable  = data.enabled && data.mainFB;

    TRACE(backend->log(AQ_LOG_TRACE,
                       std::format("atomic addConnector blobs: mode_id {}, active {}, crtc_id {}, link_status {}, content_type {}", connector->crtc->props.values.mode_id,
                                   connector->crtc->props.values.active, connector->props.values.crtc_id, connector->props.values.link_status,
                                   connector->props.values.content_type)));

    TRACE(backend->log(AQ_LOG_TRACE, std::format("atomic addConnector values: CRTC {}, mode {}", enable ? connector->crtc->id : 0, data.atomic.modeBlob)));

    // page-flips that don't change connector state must avoid touching connector_state
    // entirely, otherwise the kernel runs drm_atomic_helper_check_modeset every frame and
    // some displays (samsung HDMI TVs in particular) renegotiate the avi infoframe,
    // causing per-frame blanking. cache the last-applied values and only emit on a
    // modeset, on the first commit, or when a value actually changes. (see #265)
    const bool     forceConnProps = data.modeset || !connector->atomic.propsCached;
    const uint32_t newCrtcID      = enable ? connector->crtc->id : 0;
    uint64_t       newMaxBpc = connector->atomic.maxBpc, newColorspace = connector->atomic.colorspace;
    uint16_t       newContentType = connector->atomic.contentType;
    bool           maxBpcEmitted  = false;

    if (enable) {
        drmModeModeInfo* currentMode = connector->getCurrentMode();
        bool             modeDiffers = true;
        if (currentMode) {
            modeDiffers = memcmp(currentMode, &data.modeInfo, sizeof(drmModeModeInfo)) != 0;
            free(currentMode);
        }

        if (modeDiffers)
            addConnectorModeset(connector, data);

        // Setup HDR
        if (connector->props.values.max_bpc && connector->maxBpcBounds.at(0) && connector->maxBpcBounds.at(1)) {
            // max_bpc should not follow the scanout buffer format, and should instead always follow what we set in monitor specific configs.
            // even when in DS, we should not change the max_bpc to 8, as this will cause a modeset / flicker when going in / out of DS.
            const uint64_t WANTED = STATE.maxBpc ? STATE.maxBpc : connector->maxBpcBounds.at(1);
            const uint64_t cap    = std::clamp<uint64_t>(WANTED, connector->maxBpcBounds.at(0), connector->maxBpcBounds.at(1));
            // Cache the rejected request on fallback. The applied cap remains unknown.
            newMaxBpc = cap;
            if (!omitMaxBpc && (data.modeset || cap != connector->atomic.maxBpc || (!connector->atomic.propsCached && !connector->maxBpcFailed))) {
                add(connector->id, connector->props.values.max_bpc, cap);
                maxBpcEmitted = true;
            }
        }

        if (connector->props.values.Colorspace) {
            const auto& supported = connector->supportedColorspaces;
            const auto  selected =
                std::find_if(supported.begin(), supported.end(), [&STATE](const auto& entry) { return entry.name == (STATE.wideColorGamut ? "BT2020_RGB" : "Default"); });
            if (selected != supported.end()) {
                newColorspace = selected->value;
                if (forceConnProps || connector->atomic.colorspace != newColorspace)
                    add(connector->id, connector->props.values.Colorspace, newColorspace);
            }
        }

        if (connector->props.values.hdr_output_metadata && data.atomic.hdrd)
            add(connector->id, connector->props.values.hdr_output_metadata, data.atomic.hdrBlob);
    } else
        addConnectorModeset(connector, data);

    addConnectorCursor(connector, data);

    if (forceConnProps || connector->atomic.crtcID != newCrtcID)
        add(connector->id, connector->props.values.crtc_id, newCrtcID);

    if (enable && connector->props.values.content_type) {
        newContentType = STATE.contentType;
        if (forceConnProps || connector->atomic.contentType != newContentType)
            add(connector->id, connector->props.values.content_type, newContentType);
    }

    data.atomic.maxBpc        = newMaxBpc;
    data.atomic.maxBpcEmitted = maxBpcEmitted;
    data.atomic.colorspace    = newColorspace;
    data.atomic.contentType   = newContentType;
    data.atomic.crtcID        = newCrtcID;

    add(connector->crtc->id, connector->crtc->props.values.active, enable);

    if (enable) {
        if (connector->output->supportsExplicit && data.committed & COutputState::AQ_OUTPUT_STATE_EXPLICIT_OUT_FENCE)
            add(connector->crtc->id, connector->crtc->props.values.out_fence_ptr, (uintptr_t)&data.outputState.explicitOutFence);

        if (connector->crtc->props.values.gamma_lut && data.atomic.gammad)
            add(connector->crtc->id, connector->crtc->props.values.gamma_lut, data.atomic.gammaLut);

        if (connector->crtc->props.values.degamma_lut && data.atomic.degammad)
            add(connector->crtc->id, connector->crtc->props.values.degamma_lut, data.atomic.degammaLut);

        if (connector->crtc->props.values.ctm && data.atomic.ctmd)
            add(connector->crtc->id, connector->crtc->props.values.ctm, data.atomic.ctmBlob);

        if (connector->crtc->props.values.vrr_enabled)
            add(connector->crtc->id, connector->crtc->props.values.vrr_enabled, (uint64_t)STATE.adaptiveSync);

        planePropsInternal(connector->crtc->primary, data.mainFB, connector->crtc->id, {}, STATE.colorRange, forceConnProps || connector->atomic.colorRange != STATE.colorRange);

        if (connector->output->supportsExplicit && (data.committed & COutputState::AQ_OUTPUT_STATE_EXPLICIT_IN_FENCE) && STATE.explicitInFence >= 0)
            add(connector->crtc->primary->id, connector->crtc->primary->props.values.in_fence_fd, STATE.explicitInFence);

        if (connector->crtc->primary->props.values.fb_damage_clips)
            add(connector->crtc->primary->id, connector->crtc->primary->props.values.fb_damage_clips, data.atomic.fbDamage);
    } else {
        planeProps(connector->crtc->primary, nullptr, 0, {});
    }
}

void Aquamarine::CDRMAtomicRequest::addConnectorModeset(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data) {
    if (!data.modeset)
        return;

    const bool enable = data.enabled && data.mainFB;

    data.atomic.blobbed = true;

    if (enable) {
        add(connector->crtc->id, connector->crtc->props.values.mode_id, data.atomic.modeBlob);
        if (connector->props.values.link_status)
            add(connector->id, connector->props.values.link_status, DRM_MODE_LINK_STATUS_GOOD);
    } else
        add(connector->crtc->id, connector->crtc->props.values.mode_id, data.atomic.modeBlob);
}

void Aquamarine::CDRMAtomicRequest::addConnectorCursor(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data) {
    if (!connector->crtc->cursor)
        return;

    const bool enable = data.enabled && data.mainFB;

    if (enable) {
        if (data.committed & COutputState::AQ_OUTPUT_STATE_CURSOR_SHAPE || data.committed & COutputState::AQ_OUTPUT_STATE_CURSOR_POS) {
            TRACE(backend->log(AQ_LOG_TRACE, data.committed & COutputState::AQ_OUTPUT_STATE_CURSOR_SHAPE ? "atomic addConnector cursor shape" : "atomic addConnector cursor pos"));
            if (data.committed & COutputState::AQ_OUTPUT_STATE_CURSOR_SHAPE) {
                if (!data.cursorVisible)
                    planeProps(connector->crtc->cursor, nullptr, 0, {});
                else
                    planeProps(connector->crtc->cursor, data.cursorFB, connector->crtc->id, data.cursorPos - data.cursorHotspot);
            } else if (data.cursorVisible)
                planePropsPos(connector->crtc->cursor, data.cursorPos - data.cursorHotspot);
        }
    } else
        planeProps(connector->crtc->cursor, nullptr, 0, {});
}

bool Aquamarine::CDRMAtomicRequest::commit(uint32_t flagssss) {
    static auto flagsToStr = [](uint32_t flags) {
        std::ostringstream result;
        if (flags & DRM_MODE_ATOMIC_ALLOW_MODESET)
            result << "ATOMIC_ALLOW_MODESET ";
        if (flags & DRM_MODE_ATOMIC_NONBLOCK)
            result << "ATOMIC_NONBLOCK ";
        if (flags & DRM_MODE_ATOMIC_TEST_ONLY)
            result << "ATOMIC_TEST_ONLY ";
        if (flags & DRM_MODE_PAGE_FLIP_EVENT)
            result << "PAGE_FLIP_EVENT ";
        if (flags & DRM_MODE_PAGE_FLIP_ASYNC)
            result << "PAGE_FLIP_ASYNC ";
        if (flags & (~DRM_MODE_ATOMIC_FLAGS))
            result << " + invalid...";
        return result.str();
    };

    if (failed) {
        backend->log((flagssss & DRM_MODE_ATOMIC_TEST_ONLY) ? AQ_LOG_DEBUG : AQ_LOG_ERROR, std::format("atomic drm request: failed to commit, failed flag set to true"));
        return false;
    }

    // a test never flips.
    const bool WANTSFLIP = conn && conn->crtc && (flagssss & DRM_MODE_PAGE_FLIP_EVENT) && !(flagssss & DRM_MODE_ATOMIC_TEST_ONLY);
    const auto FLIPID    = WANTSFLIP ? conn->crtc->armPageFlip(conn, flagssss & DRM_MODE_PAGE_FLIP_ASYNC) : uintptr_t{0};

    if (auto ret = drmModeAtomicCommit(backend->gpu->fd, req, flagssss, rc<void*>(FLIPID)); ret) {
        backend->log((flagssss & DRM_MODE_ATOMIC_TEST_ONLY) ? AQ_LOG_DEBUG : AQ_LOG_ERROR,
                     std::format("atomic drm request: failed to commit: {}, flags: {}", strerror(ret == -1 ? errno : -ret), flagsToStr(flagssss)));

        if (WANTSFLIP)
            conn->crtc->disarmPageFlip();

        return false;
    }

    if (WANTSFLIP)
        conn->sched.onFrameSubmitted();

    return true;
}

void Aquamarine::CDRMAtomicRequest::destroyBlob(uint32_t id) {
    if (!id)
        return;

    if (drmModeDestroyPropertyBlob(backend->gpu->fd, id))
        backend->log(AQ_LOG_ERROR, "atomic drm request: failed to destroy a blob");
}

void Aquamarine::CDRMAtomicRequest::rollback(SDRMConnectorCommitData& data) {
    if (!conn)
        return;
    const auto destroy = [this](uint32_t id) { destroyBlob(id); };
    ColorPipeline::finishBlob(conn->crtc->atomic.modeID, data.atomic.modeBlob, data.modeset, false, destroy);
    ColorPipeline::finishBlob(conn->crtc->atomic.gammaLut, data.atomic.gammaLut, data.atomic.gammad, false, destroy);
    ColorPipeline::finishBlob(conn->crtc->atomic.degammaLut, data.atomic.degammaLut, data.atomic.degammad, false, destroy);
    ColorPipeline::finishBlob(conn->crtc->atomic.ctm, data.atomic.ctmBlob, data.atomic.ctmd, false, destroy);
    ColorPipeline::finishBlob(conn->crtc->atomic.hdr, data.atomic.hdrBlob, data.atomic.hdrd, false, destroy);
    destroyBlob(data.atomic.fbDamage);
}

void Aquamarine::CDRMAtomicRequest::apply(SDRMConnectorCommitData& data) {
    if (!conn)
        return;
    // TEST_ONLY accepts no kernel state and must release only the candidate blobs.
    if (data.test) {
        rollback(data);
        return;
    }
    if (data.atomic.blobbed && !conn->crtc->atomic.ownModeID)
        conn->crtc->atomic.modeID = 0;
    const auto destroy = [this](uint32_t id) { destroyBlob(id); };
    ColorPipeline::finishBlob(conn->crtc->atomic.modeID, data.atomic.modeBlob, data.modeset, data.atomic.blobbed, destroy);
    if (data.atomic.blobbed)
        conn->crtc->atomic.ownModeID = true;
    // Disabled CRTCs don't receive color properties in addConnector; release the candidates without adopting them.
    const bool colorApplied = data.enabled && data.mainFB;
    ColorPipeline::finishBlob(conn->crtc->atomic.gammaLut, data.atomic.gammaLut, data.atomic.gammad, colorApplied, destroy);
    ColorPipeline::finishBlob(conn->crtc->atomic.degammaLut, data.atomic.degammaLut, data.atomic.degammad, colorApplied, destroy);
    ColorPipeline::finishBlob(conn->crtc->atomic.ctm, data.atomic.ctmBlob, data.atomic.ctmd, colorApplied, destroy);
    ColorPipeline::finishBlob(conn->crtc->atomic.hdr, data.atomic.hdrBlob, data.atomic.hdrd, colorApplied, destroy);
    destroyBlob(data.atomic.fbDamage);

    // Cache the accepted connector state independently of the cap fallback.
    conn->atomic.maxBpc      = data.atomic.maxBpc;
    conn->atomic.colorspace  = data.atomic.colorspace;
    conn->atomic.contentType = data.atomic.contentType;
    conn->atomic.crtcID      = data.atomic.crtcID;
    conn->atomic.propsCached = colorApplied;
    if (data.atomic.maxBpcEmitted)
        conn->maxBpcFailed = false;
    if (colorApplied && conn->crtc->primary->props.values.color_range)
        conn->atomic.colorRange = data.outputState.colorRange;
    if (!colorApplied || data.atomic.ctmd)
        conn->crtc->atomic.ctmStateKnown = colorApplied;
}

Aquamarine::CDRMAtomicImpl::CDRMAtomicImpl(Hyprutils::Memory::CSharedPointer<CDRMBackend> backend_) : backend(backend_) {
    ;
}

CUniquePointer<CDRMAtomicRequest> Aquamarine::CDRMAtomicImpl::prepareAsync(SP<SDRMConnector> connector, SDRMConnectorCommitData& data, uint32_t& flags) {
    if (!prepareConnector(connector, data))
        return nullptr;

    auto request = makeUnique<CDRMAtomicRequest>(backend);
    request->addConnector(connector, data);

    flags = data.flags | DRM_MODE_ATOMIC_NONBLOCK;
    if (request->failed) {
        request->rollback(data);
        return nullptr;
    }

    return request;
}

void Aquamarine::CDRMAtomicImpl::finalizeAsync(CDRMAtomicRequest& request, SP<SDRMConnector> connector, SDRMConnectorCommitData& data, bool success) {
    if (!success) {
        request.rollback(data);
        return;
    }

    request.apply(data);
    connector->atomic.vrrEnabled = data.outputState.adaptiveSync;
    connector->output->vrrActive = data.outputState.adaptiveSync;
}

bool Aquamarine::CDRMAtomicImpl::prepareConnector(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data) {
    data.atomic        = {};
    const auto& STATE  = data.outputState;
    const bool  enable = data.enabled;
    const auto& MODE   = STATE.mode ? STATE.mode : STATE.customMode;

    const bool  updateGamma   = (data.modeset && connector->crtc->props.values.gamma_lut) || (data.committed & COutputState::AQ_OUTPUT_STATE_GAMMA_LUT);
    const bool  updateDegamma = (data.modeset && connector->crtc->props.values.degamma_lut) || (data.committed & COutputState::AQ_OUTPUT_STATE_DEGAMMA_LUT);
    if ((updateGamma && !ColorPipeline::validLut(STATE.gammaLut, connector->output->getGammaSize())) ||
        (updateDegamma && !ColorPipeline::validLut(STATE.degammaLut, connector->output->getDeGammaSize()))) {
        connector->backend->backend->log(AQ_LOG_ERROR, "atomic drm: unsupported LUT or LUT size mismatch");
        return false;
    }
    const bool updateCTM = ColorPipeline::shouldSubmitCTM(connector->crtc->props.values.ctm, data.modeset, data.committed & COutputState::AQ_OUTPUT_STATE_CTM,
                                                          connector->crtc->atomic.ctmStateKnown, connector->crtc->atomic.ctm, STATE.ctm == Mat3x3::identity());
    if (updateCTM && !data.ctm)
        data.ctm = STATE.ctm;

    const bool nonIdentityCTM = data.ctm && *data.ctm != Mat3x3::identity();
    const auto ctm            = nonIdentityCTM ? ColorPipeline::encodeCTM(*data.ctm) : std::optional<drm_color_ctm>{};
    if (nonIdentityCTM && (!connector->crtc->props.values.ctm || !ctm)) {
        connector->backend->backend->log(AQ_LOG_ERROR, "atomic drm: unsupported or unrepresentable CTM");
        return false;
    }

    // Callers with no scanout buffer can still validate HDR capability without allocating an unused blob.
    const bool  updateHDR = data.hdrMetadata || data.modeset || (data.committed & COutputState::AQ_OUTPUT_STATE_HDR) || !connector->atomic.propsCached;
    const auto& metadata  = data.hdrMetadata ? *data.hdrMetadata : STATE.hdrMetadata;
    const bool  hdr       = metadata.hdmi_metadata_type1.eotf != HDMI_EOTF_TRADITIONAL_GAMMA_SDR;
    if (enable && updateHDR && hdr && !connector->props.values.hdr_output_metadata) {
        connector->backend->backend->log(AQ_LOG_ERROR, "atomic drm: cannot set HDR metadata without HDR_OUTPUT_METADATA");
        return false;
    }

    bool        prepared = false;
    CScopeGuard cleanup([&] {
        if (prepared)
            return;
        CDRMAtomicRequest request(backend);
        request.setConnector(connector);
        request.rollback(data);
    });

    if (data.modeset) {
        if (!enable)
            data.atomic.modeBlob = 0;
        else {
            if (drmModeCreatePropertyBlob(connector->backend->gpu->fd, &data.modeInfo, sizeof(drmModeModeInfo), &data.atomic.modeBlob)) {
                connector->backend->backend->log(AQ_LOG_ERROR, "atomic drm: failed to create a modeset blob");
                return false;
            }

            TRACE(connector->backend->log(AQ_LOG_TRACE,
                                          std::format("Connector blob id {}: clock {}, {}x{}, vrefresh {}, name: {}", data.atomic.modeBlob, data.modeInfo.clock,
                                                      data.modeInfo.hdisplay, data.modeInfo.vdisplay, data.modeInfo.vrefresh, data.modeInfo.name)));
        }
    }

    auto prepareGammaBlob = [connector](uint32_t prop, const std::vector<uint16_t>& gammaLut, uint32_t* blobId) -> bool {
        // Without the property there is nothing to disable, addConnector skips it.
        if (!prop) {
            *blobId = 0;
            return gammaLut.empty();
        }
        if (gammaLut.empty()) {
            *blobId = 0;
            return true;
        }
        const auto lut = ColorPipeline::packLut(gammaLut);
        if (drmModeCreatePropertyBlob(connector->backend->gpu->fd, lut.data(), lut.size() * sizeof(drm_color_lut), blobId)) {
            connector->backend->backend->log(AQ_LOG_ERROR, "atomic drm: failed to create a LUT blob");
            *blobId = 0;
            return false;
        }
        return true;
    };

    // Re-send LUTs on modeset, including zero to clear a previous compositor's state.
    if (updateGamma) {
        data.atomic.gammad = prepareGammaBlob(connector->crtc->props.values.gamma_lut, STATE.gammaLut, &data.atomic.gammaLut);
        if (!data.atomic.gammad)
            return false;
    }
    if (updateDegamma) {
        data.atomic.degammad = prepareGammaBlob(connector->crtc->props.values.degamma_lut, STATE.degammaLut, &data.atomic.degammaLut);
        if (!data.atomic.degammad)
            return false;
    }

    if (enable && data.mainFB && data.ctm && connector->crtc->props.values.ctm) {
        // KMS uses a zero blob ID for a pass-through CTM.
        if (ctm && drmModeCreatePropertyBlob(connector->backend->gpu->fd, &*ctm, sizeof(*ctm), &data.atomic.ctmBlob)) {
            connector->backend->backend->log(AQ_LOG_ERROR, "atomic drm: failed to create a CTM blob");
            return false;
        }
        data.atomic.ctmd = true;
    }

    if (enable && data.mainFB && updateHDR && connector->props.values.hdr_output_metadata) {
        // SDR clears the property instead of sending an SDR DRM infoframe.
        if (hdr && drmModeCreatePropertyBlob(connector->backend->gpu->fd, &metadata, sizeof(metadata), &data.atomic.hdrBlob)) {
            connector->backend->backend->log(AQ_LOG_ERROR, "atomic drm: failed to create an HDR metadata blob");
            return false;
        }
        data.atomic.hdrd = true;
    }

    if ((data.committed & COutputState::AQ_OUTPUT_STATE_DAMAGE) && connector->crtc->primary->props.values.fb_damage_clips && MODE) {
        if (data.damage.empty())
            data.atomic.fbDamage = 0;
        else {
            TRACE(connector->backend->backend->log(AQ_LOG_TRACE, std::format("atomic drm: clipping damage to pixel size {}", MODE->pixelSize)));
            std::vector<pixman_box32_t> rects = data.damage.copy().intersect(CBox{{}, MODE->pixelSize}).getRects();
            if (drmModeCreatePropertyBlob(connector->backend->gpu->fd, rects.data(), sizeof(pixman_box32_t) * rects.size(), &data.atomic.fbDamage)) {
                connector->backend->backend->log(AQ_LOG_ERROR, "atomic drm: failed to create a damage blob");
                return false;
            }
        }
    }

    prepared = true;
    return true;
}

bool Aquamarine::CDRMAtomicImpl::commit(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data) {
    if (!prepareConnector(connector, data))
        return false;

    CDRMAtomicRequest request(backend);

    request.addConnector(connector, data);

    uint32_t flags = data.flags;
    if (data.test)
        flags |= DRM_MODE_ATOMIC_TEST_ONLY;
    if (data.modeset)
        flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
    if (!data.blocking && !data.test)
        flags |= DRM_MODE_ATOMIC_NONBLOCK;

    bool ok = request.commit(flags);

    // Only this request retries without max_bpc. A new modeset or cap can try it again.
    // Keep the candidate blobs alive for the retry and only apply the final result.
    const bool omitMaxBpc = !ok && data.atomic.maxBpcEmitted && !request.failed;
    if (omitMaxBpc) {
        connector->backend->backend->log(AQ_LOG_WARNING, "drm: atomic commit failed with max_bpc set, retrying without max_bpc");
        CDRMAtomicRequest withoutCap(backend);
        withoutCap.addConnectorInternal(connector, data, true);
        ok = withoutCap.commit(flags);
    }

    // A modeset the driver rejects may still be reachable if every head is
    // modeset together, since display resources are allocated globally. Retry
    // once that way before letting the caller fall back to a lesser mode.
    // See CDRMAtomicRequest::restateConnectors.
    if (!ok && data.modeset) {
        CDRMAtomicRequest retry(backend);
        retry.addConnectorInternal(connector, data, omitMaxBpc);
        if (retry.restateConnectors(connector)) {
            ok = retry.commit(flags);
            if (ok)
                backend->log(AQ_LOG_DEBUG,
                             std::format("atomic drm: {} modeset only passed with every head restated; the driver would not "
                                         "re-allocate for a single connector",
                                         connector->szName));
        }
    }

    // All attempts share the candidate blobs and the same connector.
    if (ok) {
        request.apply(data);
        if (!data.test && omitMaxBpc)
            connector->maxBpcFailed = true;
    } else
        request.rollback(data);

    return ok;
}

bool Aquamarine::CDRMAtomicImpl::reset() {
    CDRMAtomicRequest request(backend);

    for (auto const& crtc : backend->crtcs) {
        request.add(crtc->id, crtc->props.values.mode_id, 0);
        request.add(crtc->id, crtc->props.values.active, 0);
    }

    for (auto const& conn : backend->connectors) {
        request.add(conn->id, conn->props.values.crtc_id, 0);
    }

    for (auto const& plane : backend->planes) {
        request.planeProps(plane, nullptr, 0, {});
    }

    const bool ok = request.commit(DRM_MODE_ATOMIC_ALLOW_MODESET);
    if (ok) {
        for (auto const& crtc : backend->crtcs)
            crtc->atomic.ctmStateKnown = false;
        for (auto const& conn : backend->connectors) {
            conn->atomic.propsCached = false;
            conn->atomic.crtcID      = 0;
        }
    }

    return ok;
}

bool Aquamarine::CDRMAtomicImpl::moveCursor(SP<SDRMConnector> connector, bool skipSchedule) {
    if (!connector->output->cursorVisible || !connector->output->state->state().enabled || !connector->crtc || !connector->crtc->cursor)
        return true;

    if (!skipSchedule) {
        TRACE(connector->backend->log(AQ_LOG_TRACE, "atomic moveCursor"));
        connector->output->scheduleFrame(IOutput::AQ_SCHEDULE_CURSOR_MOVE);
    }

    return true;
}
