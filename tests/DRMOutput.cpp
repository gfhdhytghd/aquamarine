#include "CommitThread.hpp"
#include "Props.hpp"
#include "shared.hpp"

#include <algorithm>
#include <aquamarine/backend/Session.hpp>
#include <aquamarine/backend/drm/Atomic.hpp>
#include <aquamarine/backend/drm/Legacy.hpp>
#include <aquamarine/buffer/Buffer.hpp>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <ranges>
#include <unistd.h>

using namespace Aquamarine;
using namespace Hyprutils::Memory;

static constexpr uint32_t FORMAT = DRM_FORMAT_XRGB8888;
static constexpr uint32_t FB_ID = 1, CRTC_W = 2, CRTC_H = 3, MAX_BPC = 4, COLOR_RANGE = 20, CTM = 30, HDR = 31;
static constexpr uint32_t MODE_ID = 12;

using TProperties = std::map<uint32_t, uint64_t>;

struct SSubmission {
    TProperties             properties;
    uint32_t                flags = 0;

    std::optional<uint64_t> value(uint32_t property) const {
        const auto it = properties.find(property);
        return it == properties.end() ? std::nullopt : std::optional<uint64_t>{it->second};
    }
};

static std::map<drmModeAtomicReq*, TProperties> pendingProperties;
static std::vector<SSubmission>                 submissions;
static std::optional<uint64_t>                  rejectedCap;
static size_t                                   rejectedBlobSize = 0;
static size_t                                   gammaWrites      = 0;

extern "C" int                                  libseat_open_device(libseat*, const char*, int*) {
    return -1;
}

extern "C" int drmPrimeFDToHandle(int, int prime, uint32_t* handle) {
    *handle = prime;
    return 0;
}

extern "C" int drmCloseBufferHandle(int, uint32_t) {
    return 0;
}

extern "C" int drmModeAddFB2(int, uint32_t, uint32_t, uint32_t, const uint32_t[4], const uint32_t[4], const uint32_t[4], uint32_t* id, uint32_t) {
    static uint32_t nextID = 0;
    *id                    = ++nextID;
    return 0;
}

extern "C" int drmModeCloseFB(int, uint32_t) {
    return 0;
}

extern "C" int drmModeCreatePropertyBlob(int, const void*, size_t size, uint32_t* id) {
    if (size == rejectedBlobSize)
        return -ENOMEM;
    static uint32_t nextID = 0;
    *id                    = ++nextID;
    return 0;
}

extern "C" int drmModeDestroyPropertyBlob(int, uint32_t) {
    return 0;
}

extern "C" drmModeObjectPropertiesPtr drmModeObjectGetProperties(int, uint32_t, uint32_t) {
    return nullptr; // Connector and plane property readback is unavailable.
}

extern "C" int drmModeCrtcSetGamma(int, uint32_t, uint32_t size, const uint16_t*, const uint16_t*, const uint16_t*) {
    ++gammaWrites;
    return size ? 0 : -ENOSYS;
}

extern "C" drmModeCrtcPtr drmModeGetCrtc(int, uint32_t) {
    return nullptr;
}

extern "C" int drmModeAtomicAddProperty(drmModeAtomicReq* req, uint32_t, uint32_t prop, uint64_t value) {
    pendingProperties[req][prop] = value;
    return 0;
}

extern "C" int drmModeAtomicCommit(int, drmModeAtomicReq* req, uint32_t flags, void*) {
    submissions.push_back({.properties = std::move(pendingProperties[req]), .flags = flags});
    pendingProperties.erase(req);
    if (rejectedCap && submissions.back().value(MAX_BPC) == rejectedCap) {
        errno = EINVAL;
        return -EINVAL;
    }
    return 0;
}

class CMockBuffer : public IBuffer {
  public:
    CMockBuffer() : m_attrs{.success = true, .size = {1920, 1080}, .format = FORMAT, .modifier = DRM_FORMAT_MOD_LINEAR, .fds = {1, -1, -1, -1}} {
        size = m_attrs.size;
    }
    eBufferCapability caps() override {
        return BUFFER_CAPABILITY_NONE;
    }
    eBufferType type() override {
        return BUFFER_TYPE_DMABUF;
    }
    void update(const Hyprutils::Math::CRegion&) override {
        ;
    }
    bool isSynchronous() override {
        return true;
    }
    bool good() override {
        return true;
    }
    SDMABUFAttrs dmabuf() override {
        return m_attrs;
    }

  private:
    SDMABUFAttrs m_attrs;
};

struct SMockDRM {
    SMockDRM() {
        SBackendImplementationOptions options;
        options.backendType       = AQ_BACKEND_NULL;
        backend                   = CBackend::create({options}, SBackendOptions{});
        backend->session          = makeShared<CSession>();
        backend->session->backend = backend;
        drm                       = CSharedPointer<CDRMBackend>(new CDRMBackend(backend));
        drm->self                 = drm;
        drm->gpu                  = makeShared<CSessionDevice>(backend->session, "mock");
        drm->gpu->fd              = open("/dev/null", O_RDONLY | O_CLOEXEC);
        drm->atomic               = true;
        drm->impl                 = makeShared<CDRMAtomicImpl>(drm);
        drm->rendererRequired     = false;

        connector                             = makeShared<SDRMConnector>();
        connector->self                       = connector;
        connector->backend                    = drm;
        connector->id                         = 1;
        connector->props.values.crtc_id       = 10;
        connector->crtc                       = makeShared<SDRMCRTC>();
        connector->crtc->id                   = 2;
        connector->crtc->backend              = drm;
        connector->crtc->props.values.active  = 11;
        connector->crtc->props.values.mode_id = MODE_ID;
        connector->crtc->primary              = makeShared<SDRMPlane>();
        auto& plane                           = *connector->crtc->primary;
        plane.id                              = 3;
        plane.formats                         = {{.drmFormat = FORMAT}};
        plane.props.values.fb_id              = FB_ID;
        plane.props.values.crtc_w             = CRTC_W;
        plane.props.values.crtc_h             = CRTC_H;
        drm->connectors.push_back(connector);
        backend->ready = true;
        drmModeModeInfo  mode{.clock = 148500, .hdisplay = 1920, .htotal = 2200, .vdisplay = 1080, .vtotal = 1125};
        drmModeConnector info{.count_modes = 1, .modes = &mode};
        connector->connect(&info);
        connector->output->state->setMode(connector->output->modes.front());
        connector->output->state->setEnabled(true);
        connector->output->state->setFormat(FORMAT);
    }

    CSharedPointer<CBackend>      backend;
    CSharedPointer<CDRMBackend>   drm;
    CSharedPointer<SDRMConnector> connector;
};

static int testMaxBpcFallback() {
    int         ret = 0;
    SMockDRM    mock;
    const auto& conn           = mock.connector;
    const auto& output         = conn->output;
    conn->props.values.max_bpc = MAX_BPC;
    conn->maxBpcBounds         = {8, 12};
    conn->atomic.maxBpc        = 8;
    output->state->setBuffer(makeShared<CMockBuffer>());
    submissions.clear();
    rejectedCap = 12;

    EXPECT(output->test(), true);
    EXPECT(conn->maxBpcFailed, false);
    EXPECT(conn->atomic.propsCached, false);
    submissions.clear();

    EXPECT(output->commit(), true);
    EXPECT(submissions.size(), 2U);
    if (submissions.size() != 2)
        return 1;
    EXPECT(submissions.front().value(MAX_BPC).value_or(0), 12U);
    EXPECT(submissions.back().value(MAX_BPC).has_value(), false);
    EXPECT(conn->maxBpcFailed, true);
    EXPECT(conn->atomic.propsCached, true);

    conn->invalidateFrame();
    submissions.clear();
    output->state->setBuffer(makeShared<CMockBuffer>());
    EXPECT(output->state->needsReconfig(), false);
    EXPECT(output->commit(), true);
    EXPECT(submissions.size(), 1U);
    if (submissions.size() != 1)
        return 1;
    EXPECT(submissions.front().value(MAX_BPC).has_value(), false);

    // Submit a real queued buffer-only flip while the driver still rejects the old cap.
    conn->invalidateFrame();
    if (!mock.drm->initCommitThread())
        return 1;
    std::optional<IOutput::SCommitResult> result;
    const auto                            listener = output->events.commitResult.listen([&](IOutput::SCommitResult event) { result = event; });
    output->state->setBuffer(makeShared<CMockBuffer>());
    const auto queued = output->commitAsync({});
    EXPECT(queued.error, 0);
    if (!queued.id)
        return 1;

    pollfd     completion{.fd = mock.drm->commitThread->completionFD(), .events = POLLIN};
    const auto pollResult = poll(&completion, 1, 2000);
    EXPECT(pollResult, 1);
    if (pollResult != 1)
        return 1;
    mock.drm->flushAsyncCommitEvents();
    mock.drm->dispatchCommitResults();
    EXPECT(result.has_value(), true);
    if (!result)
        return 1;
    EXPECT(result->id, queued.id);
    EXPECT(result->status, IOutput::AQ_OUTPUT_COMMIT_SUBMITTED);
    EXPECT(submissions.size(), 2U);
    EXPECT(submissions.back().value(MAX_BPC).has_value(), false);
    EXPECT(conn->maxBpcFailed, true);
    EXPECT(conn->atomic.propsCached, true);
    conn->invalidateFrame();
    mock.drm->stopCommitThread();

    // A different cap can succeed without reconnecting.
    output->state->setMaxBpc(10);
    submissions.clear();
    EXPECT(output->commit(), true);
    EXPECT(submissions.size(), 1U);
    EXPECT(submissions.back().value(MAX_BPC).value_or(0), 10U);
    EXPECT(conn->maxBpcFailed, false);

    // Reject the next cap, then accept it on a new modeset with the same request.
    conn->invalidateFrame();
    output->state->setMaxBpc(12);
    EXPECT(output->commit(), true);
    EXPECT(conn->maxBpcFailed, true);
    conn->invalidateFrame();
    rejectedCap.reset();
    output->state->setMode(output->modes.front());
    submissions.clear();
    EXPECT(output->commit(), true);
    EXPECT(submissions.size(), 1U);
    EXPECT(submissions.back().value(MODE_ID).has_value(), true);
    EXPECT(submissions.back().value(MAX_BPC).value_or(0), 12U);
    EXPECT(conn->maxBpcFailed, false);
    return ret;
}

static int testColorRange() {
    int         ret = 0;
    SMockDRM    mock;
    const auto& conn                = mock.connector;
    const auto& output              = conn->output;
    const auto  plane               = conn->crtc->primary;
    plane->props.values.color_range = COLOR_RANGE;
    mock.drm->planes.push_back(plane);
    constexpr uint64_t full = uint64_t{1} << 40;
    rememberDRMPlaneColorRange(*plane, {.full = full, .limited = 0, .automatic = 0});
    output->state->setBuffer(makeShared<CMockBuffer>());
    EXPECT(output->commit(), true);
    EXPECT(submissions.back().value(COLOR_RANGE).value_or(99), 0U);

    conn->invalidateFrame();
    output->state->setBuffer(makeShared<CMockBuffer>());
    EXPECT(output->state->needsReconfig(), false);
    EXPECT(output->commit(), true);
    EXPECT(submissions.back().value(COLOR_RANGE).has_value(), false);

    conn->invalidateFrame();
    output->state->setColorRange(AQ_OUTPUT_COLOR_RANGE_FULL);
    EXPECT(output->commit(), true);
    EXPECT(submissions.back().value(COLOR_RANGE).value_or(0), full);

    conn->invalidateFrame();
    output->state->setColorRange(AQ_OUTPUT_COLOR_RANGE_AUTO);
    EXPECT(output->commit(), true);
    EXPECT(submissions.back().value(COLOR_RANGE).value_or(99), 0U);

    conn->invalidateFrame();
    conn->atomic.propsCached = false;
    mock.drm->driver         = AQ_BACKEND_GPU_DRIVER_NVIDIA;
    output->state->setBuffer(makeShared<CMockBuffer>());
    EXPECT(output->commit(), true);
    EXPECT(submissions.back().value(COLOR_RANGE).value_or(0), full);
    return ret;
}

static int testColorReplay() {
    int        ret = 0;
    SMockDRM   mock;
    auto&      conn                        = mock.connector;
    const auto output                      = conn->output;
    conn->crtc->props.values.ctm           = CTM;
    conn->props.values.hdr_output_metadata = HDR;
    auto coefficients                      = Hyprutils::Math::Mat3x3::identity().getMatrix();
    coefficients[0]                        = 0.5;
    const hdr_output_metadata pq{.hdmi_metadata_type1 = {.eotf = 2}};
    output->state->setCTM(Hyprutils::Math::Mat3x3{coefficients});
    output->state->setHDRMetadata(pq);
    output->state->setBuffer(makeShared<CMockBuffer>());
    EXPECT(output->test(), true);
    EXPECT(submissions.back().flags & DRM_MODE_ATOMIC_TEST_ONLY, DRM_MODE_ATOMIC_TEST_ONLY);
    EXPECT(submissions.back().value(CTM).value_or(0) != 0, true);
    EXPECT(submissions.back().value(HDR).value_or(0) != 0, true);
    EXPECT(conn->atomic.propsCached, false);
    EXPECT(conn->crtc->atomic.ctmStateKnown, false);
    EXPECT(conn->sched.frameInFlight(), false);
    EXPECT(output->commit(), true);

    output->state->setEnabled(false);
    output->state->setCTM(Hyprutils::Math::Mat3x3::identity());
    output->state->setHDRMetadata({});
    EXPECT(output->commit(), true);
    EXPECT(submissions.back().value(CTM).has_value(), false);
    EXPECT(submissions.back().value(HDR).has_value(), false);
    output->state->setEnabled(true);
    EXPECT(output->commit(), true);
    EXPECT(submissions.back().value(CTM).value_or(999), 0U);
    EXPECT(submissions.back().value(HDR).value_or(999), 0U);
    EXPECT(conn->crtc->atomic.ctmStateKnown, true);

    const auto writes = submissions.size();
    output->state->setHDRMetadata(pq);
    rejectedBlobSize = sizeof(hdr_output_metadata);
    EXPECT(output->commit(), false);
    EXPECT(submissions.size(), writes);
    EXPECT(conn->crtc->atomic.hdr, 0U);
    EXPECT(bool(output->state->state().committed & COutputState::AQ_OUTPUT_STATE_HDR), true);
    rejectedBlobSize                       = 0;
    conn->props.values.hdr_output_metadata = 0;
    EXPECT(output->test(), false);
    EXPECT(submissions.size(), writes);
    return ret;
}

static int testSteadyFlips() {
    int         ret = 0;
    SMockDRM    mock;
    const auto& conn                       = mock.connector;
    const auto& output                     = conn->output;
    conn->crtc->props.values.ctm           = CTM;
    conn->props.values.hdr_output_metadata = HDR;
    const hdr_output_metadata pq{.hdmi_metadata_type1 = {.eotf = 2}};
    output->state->setBuffer(makeShared<CMockBuffer>());
    // HEAD programs active/VRR, plane geometry, FB, CRTC and damage on a
    // buffer-only flip. It omits unchanged connector properties and colour blobs.
    conn->props.values.max_bpc           = MAX_BPC;
    conn->maxBpcBounds                   = {8, 12};
    conn->props.values.content_type      = 32;
    conn->props.values.Colorspace        = 33;
    conn->supportedColorspaces           = {{.name = "Default", .value = 0}, {.name = "BT2020_RGB", .value = 1}};
    conn->crtc->props.values.gamma_lut   = 34;
    conn->crtc->props.values.degamma_lut = 35;
    conn->crtc->props.values.vrr_enabled = 36;
    auto& plane                          = conn->crtc->primary->props.values;
    plane.src_x                          = 37;
    plane.src_y                          = 38;
    plane.src_w                          = 39;
    plane.src_h                          = 40;
    plane.crtc_id                        = 41;
    plane.crtc_x                         = 42;
    plane.crtc_y                         = 43;
    plane.fb_damage_clips                = 44;

    for (const auto& metadata : {hdr_output_metadata{}, pq}) {
        conn->invalidateFrame();
        output->state->setHDRMetadata(metadata);
        output->state->setMode(output->modes.front());
        EXPECT(output->commit(), true);
        conn->invalidateFrame();
        output->state->setBuffer(makeShared<CMockBuffer>());
        const auto before = submissions.size();
        EXPECT(output->commit(), true);
        EXPECT(submissions.size(), before + 1);
        const auto&                 flip     = submissions.back();
        const std::vector<uint32_t> expected = {FB_ID, CRTC_W, CRTC_H, 11, 36, 37, 38, 39, 40, 41, 42, 43, 44};
        EXPECT(std::ranges::equal(flip.properties | std::views::keys, expected), true);
        EXPECT(bool(flip.flags & DRM_MODE_ATOMIC_ALLOW_MODESET), false);
    }
    return ret;
}

static int testZeroSizeLegacyGamma() {
    int      ret = 0;
    SMockDRM mock;
    mock.drm->atomic = false;
    CDRMLegacyImpl          legacy(mock.drm);
    SDRMConnectorCommitData data{.enabled = true, .committed = COutputState::AQ_OUTPUT_STATE_GAMMA_LUT};
    EXPECT(mock.connector->output->getGammaSize(), 0U);
    EXPECT(legacy.commit(mock.connector, data), true);
    EXPECT(gammaWrites, 0U);
    data.outputState.gammaLut = {1, 2, 3};
    EXPECT(legacy.commit(mock.connector, data), false);
    EXPECT(gammaWrites, 0U);
    return ret;
}

int main() {
    return testMaxBpcFallback() | testColorRange() | testColorReplay() | testSteadyFlips() | testZeroSizeLegacyGamma();
}
