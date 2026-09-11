#include "Camera/CameraLink.hpp"

#include "Core/VrCoreShared.hpp"

#include <atomic>
#include <cstdint>

// Lifted out of the core hub verbatim when the three camera hooks were split into files of their
// own. The arithmetic is untouched; what changed is that the raw objects below are file-local, so
// the seqlock and the ring can only be reached through the four functions the header declares.
//
// See Camera/CameraLink.hpp for the ownership rule, the single-publisher invariant, and why these
// two are functions where LiveControls' forty scalars are not.

namespace {
std::atomic<uint32_t> g_camWriteSeq{0};   // even = stable, odd = write in progress

float g_camWriteQuat[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
}  // namespace

namespace {
struct AtomicFinalMainCameraFrame {
    std::atomic<float> worldPos[3]{};
    std::atomic<float> worldQuat[4]{};
    std::atomic<float> hmdPos[3]{};
    std::atomic<float> hmdOri[4]{};
    std::atomic<float> recenterPos[3]{};
    std::atomic<float> recenterOri[4]{};
    std::atomic<uint64_t> hmdFrameAimEpoch{0};
    std::atomic<uint32_t> hmdValid{0};
    std::atomic<uint32_t> hmdRecenterBaseValid{0};
    std::atomic<uint64_t> timestampUs{0};
    std::atomic<uint64_t> callbackHit{0};
    std::atomic<uint32_t> locateSequence{0};
    std::atomic<uint32_t> sequence{0};
    std::atomic<uint32_t> hmdComposed{0};
};

AtomicFinalMainCameraFrame g_finalMain{};
std::atomic<uint32_t> g_finalMainSeq{0};

struct AtomicGenericNonFppCameraFrame {
    std::atomic<float> worldPos[3]{};
    std::atomic<float> worldQuat[4]{};
    std::atomic<float> hmdPos[3]{};
    std::atomic<float> hmdOri[4]{};
    std::atomic<float> recenterPos[3]{};
    std::atomic<float> recenterOri[4]{};
    std::atomic<uint64_t> hmdFrameAimEpoch{0};
    std::atomic<uint32_t> hmdValid{0};
    std::atomic<uint32_t> hmdRecenterBaseValid{0};
    std::atomic<uint64_t> timestampUs{0};
    std::atomic<uint32_t> sequence{0};
    std::atomic<uint32_t> active{0};
    std::atomic<uint32_t> hmdComposed{0};
};

AtomicGenericNonFppCameraFrame g_genericNonFppFrame{};
std::atomic<uint32_t> g_genericNonFppFrameSeq{0};
std::atomic<bool> g_genericNonFppActive{false};
thread_local cvr::camera::GenericVrcamLocateScope g_genericVrcamLocateScope{};

struct CameraDirectorBlendScopeEntry {
    uintptr_t cameraObject = 0;
    OpenXRHeadPose head{};
    bool composed = false;
};

struct CameraDirectorBlendScopeState {
    CameraDirectorBlendScopeEntry entries[8]{};
    OpenXRHeadPose preferredHead{};
    uint32_t count = 0;
    bool active = false;
    bool preferredHeadValid = false;
};

thread_local CameraDirectorBlendScopeState g_cameraDirectorBlendScope{};


}  // namespace

void cvr::camera::FinalMainCameraFramePublish(const FinalMainCameraFrame& f) {
    g_finalMainSeq.fetch_add(1u, std::memory_order_acq_rel);
    for (int i = 0; i < 3; ++i) {
        g_finalMain.worldPos[i].store(f.worldPos[i], std::memory_order_relaxed);
    }
    for (int i = 0; i < 4; ++i) {
        g_finalMain.worldQuat[i].store(f.worldQuat[i], std::memory_order_relaxed);
    }
    g_finalMain.hmdPos[0].store(f.hmdPose.posX, std::memory_order_relaxed);
    g_finalMain.hmdPos[1].store(f.hmdPose.posY, std::memory_order_relaxed);
    g_finalMain.hmdPos[2].store(f.hmdPose.posZ, std::memory_order_relaxed);
    g_finalMain.hmdOri[0].store(f.hmdPose.oriX, std::memory_order_relaxed);
    g_finalMain.hmdOri[1].store(f.hmdPose.oriY, std::memory_order_relaxed);
    g_finalMain.hmdOri[2].store(f.hmdPose.oriZ, std::memory_order_relaxed);
    g_finalMain.hmdOri[3].store(f.hmdPose.oriW, std::memory_order_relaxed);
    g_finalMain.recenterPos[0].store(f.hmdPose.recenterBase.position.x, std::memory_order_relaxed);
    g_finalMain.recenterPos[1].store(f.hmdPose.recenterBase.position.y, std::memory_order_relaxed);
    g_finalMain.recenterPos[2].store(f.hmdPose.recenterBase.position.z, std::memory_order_relaxed);
    g_finalMain.recenterOri[0].store(f.hmdPose.recenterBase.orientation.x, std::memory_order_relaxed);
    g_finalMain.recenterOri[1].store(f.hmdPose.recenterBase.orientation.y, std::memory_order_relaxed);
    g_finalMain.recenterOri[2].store(f.hmdPose.recenterBase.orientation.z, std::memory_order_relaxed);
    g_finalMain.recenterOri[3].store(f.hmdPose.recenterBase.orientation.w, std::memory_order_relaxed);
    g_finalMain.hmdFrameAimEpoch.store(f.hmdPose.frameAimEpoch, std::memory_order_relaxed);
    g_finalMain.hmdValid.store(f.hmdPose.valid ? 1u : 0u, std::memory_order_relaxed);
    g_finalMain.hmdRecenterBaseValid.store(f.hmdPose.recenterBaseValid ? 1u : 0u,
                                           std::memory_order_relaxed);
    g_finalMain.timestampUs.store(f.timestampUs, std::memory_order_relaxed);
    g_finalMain.callbackHit.store(f.callbackHit, std::memory_order_relaxed);
    g_finalMain.locateSequence.store(f.locateSequence, std::memory_order_relaxed);
    g_finalMain.sequence.store(f.sequence, std::memory_order_relaxed);
    g_finalMain.hmdComposed.store(f.hmdComposed, std::memory_order_relaxed);
    g_finalMainSeq.fetch_add(1u, std::memory_order_release);
}

bool cvr::camera::FinalMainCameraFrameRead(FinalMainCameraFrame* out) {
    if (!out) return false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t s0 = g_finalMainSeq.load(std::memory_order_acquire);
        if (s0 == 0u || (s0 & 1u)) continue;

        FinalMainCameraFrame tmp{};
        for (int i = 0; i < 3; ++i) {
            tmp.worldPos[i] = g_finalMain.worldPos[i].load(std::memory_order_relaxed);
        }
        for (int i = 0; i < 4; ++i) {
            tmp.worldQuat[i] = g_finalMain.worldQuat[i].load(std::memory_order_relaxed);
        }
        tmp.hmdPose.posX = g_finalMain.hmdPos[0].load(std::memory_order_relaxed);
        tmp.hmdPose.posY = g_finalMain.hmdPos[1].load(std::memory_order_relaxed);
        tmp.hmdPose.posZ = g_finalMain.hmdPos[2].load(std::memory_order_relaxed);
        tmp.hmdPose.oriX = g_finalMain.hmdOri[0].load(std::memory_order_relaxed);
        tmp.hmdPose.oriY = g_finalMain.hmdOri[1].load(std::memory_order_relaxed);
        tmp.hmdPose.oriZ = g_finalMain.hmdOri[2].load(std::memory_order_relaxed);
        tmp.hmdPose.oriW = g_finalMain.hmdOri[3].load(std::memory_order_relaxed);
        tmp.hmdPose.recenterBase.position.x = g_finalMain.recenterPos[0].load(std::memory_order_relaxed);
        tmp.hmdPose.recenterBase.position.y = g_finalMain.recenterPos[1].load(std::memory_order_relaxed);
        tmp.hmdPose.recenterBase.position.z = g_finalMain.recenterPos[2].load(std::memory_order_relaxed);
        tmp.hmdPose.recenterBase.orientation.x = g_finalMain.recenterOri[0].load(std::memory_order_relaxed);
        tmp.hmdPose.recenterBase.orientation.y = g_finalMain.recenterOri[1].load(std::memory_order_relaxed);
        tmp.hmdPose.recenterBase.orientation.z = g_finalMain.recenterOri[2].load(std::memory_order_relaxed);
        tmp.hmdPose.recenterBase.orientation.w = g_finalMain.recenterOri[3].load(std::memory_order_relaxed);
        tmp.hmdPose.frameAimEpoch = g_finalMain.hmdFrameAimEpoch.load(std::memory_order_relaxed);
        tmp.hmdPose.valid = g_finalMain.hmdValid.load(std::memory_order_relaxed) != 0;
        tmp.hmdPose.recenterBaseValid =
            g_finalMain.hmdRecenterBaseValid.load(std::memory_order_relaxed) != 0;
        tmp.timestampUs = g_finalMain.timestampUs.load(std::memory_order_relaxed);
        tmp.callbackHit = g_finalMain.callbackHit.load(std::memory_order_relaxed);
        tmp.locateSequence = g_finalMain.locateSequence.load(std::memory_order_relaxed);
        tmp.sequence = g_finalMain.sequence.load(std::memory_order_relaxed);
        tmp.hmdComposed = g_finalMain.hmdComposed.load(std::memory_order_relaxed);

        if (g_finalMainSeq.load(std::memory_order_acquire) == s0) {
            *out = tmp;
            return true;
        }
    }
    return false;
}

void cvr::camera::GenericNonFppCameraFramePublish(const GenericNonFppCameraFrame& f) {
    g_genericNonFppFrameSeq.fetch_add(1u, std::memory_order_acq_rel);
    for (int i = 0; i < 3; ++i) {
        g_genericNonFppFrame.worldPos[i].store(f.worldPos[i], std::memory_order_relaxed);
    }
    for (int i = 0; i < 4; ++i) {
        g_genericNonFppFrame.worldQuat[i].store(f.worldQuat[i], std::memory_order_relaxed);
    }
    g_genericNonFppFrame.hmdPos[0].store(f.hmdPose.posX, std::memory_order_relaxed);
    g_genericNonFppFrame.hmdPos[1].store(f.hmdPose.posY, std::memory_order_relaxed);
    g_genericNonFppFrame.hmdPos[2].store(f.hmdPose.posZ, std::memory_order_relaxed);
    g_genericNonFppFrame.hmdOri[0].store(f.hmdPose.oriX, std::memory_order_relaxed);
    g_genericNonFppFrame.hmdOri[1].store(f.hmdPose.oriY, std::memory_order_relaxed);
    g_genericNonFppFrame.hmdOri[2].store(f.hmdPose.oriZ, std::memory_order_relaxed);
    g_genericNonFppFrame.hmdOri[3].store(f.hmdPose.oriW, std::memory_order_relaxed);
    g_genericNonFppFrame.recenterPos[0].store(f.hmdPose.recenterBase.position.x, std::memory_order_relaxed);
    g_genericNonFppFrame.recenterPos[1].store(f.hmdPose.recenterBase.position.y, std::memory_order_relaxed);
    g_genericNonFppFrame.recenterPos[2].store(f.hmdPose.recenterBase.position.z, std::memory_order_relaxed);
    g_genericNonFppFrame.recenterOri[0].store(f.hmdPose.recenterBase.orientation.x, std::memory_order_relaxed);
    g_genericNonFppFrame.recenterOri[1].store(f.hmdPose.recenterBase.orientation.y, std::memory_order_relaxed);
    g_genericNonFppFrame.recenterOri[2].store(f.hmdPose.recenterBase.orientation.z, std::memory_order_relaxed);
    g_genericNonFppFrame.recenterOri[3].store(f.hmdPose.recenterBase.orientation.w, std::memory_order_relaxed);
    g_genericNonFppFrame.hmdFrameAimEpoch.store(f.hmdPose.frameAimEpoch, std::memory_order_relaxed);
    g_genericNonFppFrame.hmdValid.store(f.hmdPose.valid ? 1u : 0u, std::memory_order_relaxed);
    g_genericNonFppFrame.hmdRecenterBaseValid.store(f.hmdPose.recenterBaseValid ? 1u : 0u,
                                                    std::memory_order_relaxed);
    g_genericNonFppFrame.timestampUs.store(f.timestampUs, std::memory_order_relaxed);
    g_genericNonFppFrame.sequence.store(f.sequence, std::memory_order_relaxed);
    g_genericNonFppFrame.active.store(f.active, std::memory_order_relaxed);
    g_genericNonFppFrame.hmdComposed.store(f.hmdComposed, std::memory_order_relaxed);
    g_genericNonFppFrameSeq.fetch_add(1u, std::memory_order_release);
}

bool cvr::camera::GenericNonFppCameraFrameRead(GenericNonFppCameraFrame* out) {
    if (!out) return false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t s0 = g_genericNonFppFrameSeq.load(std::memory_order_acquire);
        if (s0 == 0u || (s0 & 1u)) continue;

        GenericNonFppCameraFrame tmp{};
        for (int i = 0; i < 3; ++i) {
            tmp.worldPos[i] = g_genericNonFppFrame.worldPos[i].load(std::memory_order_relaxed);
        }
        for (int i = 0; i < 4; ++i) {
            tmp.worldQuat[i] = g_genericNonFppFrame.worldQuat[i].load(std::memory_order_relaxed);
        }
        tmp.hmdPose.posX = g_genericNonFppFrame.hmdPos[0].load(std::memory_order_relaxed);
        tmp.hmdPose.posY = g_genericNonFppFrame.hmdPos[1].load(std::memory_order_relaxed);
        tmp.hmdPose.posZ = g_genericNonFppFrame.hmdPos[2].load(std::memory_order_relaxed);
        tmp.hmdPose.oriX = g_genericNonFppFrame.hmdOri[0].load(std::memory_order_relaxed);
        tmp.hmdPose.oriY = g_genericNonFppFrame.hmdOri[1].load(std::memory_order_relaxed);
        tmp.hmdPose.oriZ = g_genericNonFppFrame.hmdOri[2].load(std::memory_order_relaxed);
        tmp.hmdPose.oriW = g_genericNonFppFrame.hmdOri[3].load(std::memory_order_relaxed);
        tmp.hmdPose.recenterBase.position.x = g_genericNonFppFrame.recenterPos[0].load(std::memory_order_relaxed);
        tmp.hmdPose.recenterBase.position.y = g_genericNonFppFrame.recenterPos[1].load(std::memory_order_relaxed);
        tmp.hmdPose.recenterBase.position.z = g_genericNonFppFrame.recenterPos[2].load(std::memory_order_relaxed);
        tmp.hmdPose.recenterBase.orientation.x = g_genericNonFppFrame.recenterOri[0].load(std::memory_order_relaxed);
        tmp.hmdPose.recenterBase.orientation.y = g_genericNonFppFrame.recenterOri[1].load(std::memory_order_relaxed);
        tmp.hmdPose.recenterBase.orientation.z = g_genericNonFppFrame.recenterOri[2].load(std::memory_order_relaxed);
        tmp.hmdPose.recenterBase.orientation.w = g_genericNonFppFrame.recenterOri[3].load(std::memory_order_relaxed);
        tmp.hmdPose.frameAimEpoch = g_genericNonFppFrame.hmdFrameAimEpoch.load(std::memory_order_relaxed);
        tmp.hmdPose.valid = g_genericNonFppFrame.hmdValid.load(std::memory_order_relaxed) != 0;
        tmp.hmdPose.recenterBaseValid =
            g_genericNonFppFrame.hmdRecenterBaseValid.load(std::memory_order_relaxed) != 0;
        tmp.timestampUs = g_genericNonFppFrame.timestampUs.load(std::memory_order_relaxed);
        tmp.sequence = g_genericNonFppFrame.sequence.load(std::memory_order_relaxed);
        tmp.active = g_genericNonFppFrame.active.load(std::memory_order_relaxed);
        tmp.hmdComposed = g_genericNonFppFrame.hmdComposed.load(std::memory_order_relaxed);

        if (g_genericNonFppFrameSeq.load(std::memory_order_acquire) == s0) {
            *out = tmp;
            return true;
        }
    }
    return false;
}

void cvr::camera::GenericNonFppActivePublish(bool active) {
    g_genericNonFppActive.store(active, std::memory_order_release);
}

bool cvr::camera::GenericNonFppActiveRead() {
    return g_genericNonFppActive.load(std::memory_order_acquire);
}

cvr::camera::GenericVrcamLocateScope cvr::camera::GenericVrcamLocateScopeExchange(
    const GenericVrcamLocateScope& scope) {
    const GenericVrcamLocateScope previous = g_genericVrcamLocateScope;
    g_genericVrcamLocateScope = scope;
    return previous;
}

bool cvr::camera::GenericVrcamLocateScopeMatches(uintptr_t cameraObject) {
    return g_genericVrcamLocateScope.active && cameraObject != 0 &&
           g_genericVrcamLocateScope.cameraObject == cameraObject;
}

bool cvr::camera::GenericVrcamLocateScopeEntryAlreadyComposed(uintptr_t cameraObject) {
    return GenericVrcamLocateScopeMatches(cameraObject) &&
           g_genericVrcamLocateScope.entryAlreadyComposed;
}

bool cvr::camera::GenericVrcamLocateScopeRead(uintptr_t cameraObject, OpenXRHeadPose* outHead) {
    if (!outHead || !GenericVrcamLocateScopeMatches(cameraObject)) return false;
    *outHead = g_genericVrcamLocateScope.head;
    g_genericVrcamLocateScope.locateConsumed = true;
    return true;
}

void cvr::camera::CamWriteQuatPublish(float x, float y, float z, float w) {
    g_camWriteSeq.fetch_add(1, std::memory_order_acq_rel);
    std::atomic_thread_fence(std::memory_order_release);
    g_camWriteQuat[0] = x; g_camWriteQuat[1] = y;
    g_camWriteQuat[2] = z; g_camWriteQuat[3] = w;
    std::atomic_thread_fence(std::memory_order_release);
    g_camWriteSeq.fetch_add(1, std::memory_order_acq_rel);
}

bool cvr::camera::CamWriteQuatRead(float out[4]) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t s0 = g_camWriteSeq.load(std::memory_order_acquire);
        if (s0 == 0 || (s0 & 1u)) continue;          // never published / mid-write
        float tmp[4] = { g_camWriteQuat[0], g_camWriteQuat[1],
                         g_camWriteQuat[2], g_camWriteQuat[3] };
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_camWriteSeq.load(std::memory_order_acquire) == s0) {
            out[0] = tmp[0]; out[1] = tmp[1]; out[2] = tmp[2]; out[3] = tmp[3];
            return true;
        }
    }
    return false;
}

// ---- WHAT WE WROTE, SO IT CAN BE RECOGNISED LATER -------------------------------------------
//
// Every composed quaternion is filed here next to the XR sample it came from. The render-side
// hook then reads the quaternion the engine is about to render with and finds it in this ring,
// which identifies the frame's pose exactly -- no assumption about how far ahead the engine
// renders, and immune to the repeats and skips that phase drift between the simulation thread
// and our aim epoch produces. See OpenXRManager::PushRenderedFramePose.
namespace {
struct CamWriteRecord {
    float quat[4];
    OpenXRHeadPose pose;
    uint64_t id;         // monotonic write index; ordering is what disambiguates a tie
    uint32_t valid;
};

constexpr uint32_t kCamWriteRing = 16;

CamWriteRecord g_camWriteRing[kCamWriteRing]{};

std::atomic<uint64_t> g_camWriteRingHead{0};
}  // namespace

void cvr::camera::CamWriteRecordPush(const float q[4], const OpenXRHeadPose& p) {
    const uint64_t id = g_camWriteRingHead.load(std::memory_order_relaxed);
    CamWriteRecord& r = g_camWriteRing[id % kCamWriteRing];
    r.valid = 0;
    std::atomic_thread_fence(std::memory_order_release);
    r.quat[0] = q[0]; r.quat[1] = q[1]; r.quat[2] = q[2]; r.quat[3] = q[3];
    r.pose = p;
    r.id = id;
    std::atomic_thread_fence(std::memory_order_release);
    r.valid = 1;
    g_camWriteRingHead.fetch_add(1, std::memory_order_release);
}

bool cvr::camera::CamWriteRecordFindExact(const float q[4], OpenXRHeadPose* out) {
    if (!q) return false;
    const uint64_t head = g_camWriteRingHead.load(std::memory_order_acquire);
    const uint64_t n = head < kCamWriteRing ? head : kCamWriteRing;
    for (uint64_t i = 1; i <= n; ++i) {
        const CamWriteRecord& r = g_camWriteRing[(head - i) % kCamWriteRing];
        if (!r.valid) continue;
        if (r.quat[0] == q[0] && r.quat[1] == q[1] &&
            r.quat[2] == q[2] && r.quat[3] == q[3]) {
            if (out) *out = r.pose;
            return true;
        }
    }
    return false;
}

namespace {
bool SameHeadSample(const OpenXRHeadPose& a, const OpenXRHeadPose& b) {
    if (!a.valid || !b.valid || a.frameAimEpoch != b.frameAimEpoch ||
        a.recenterBaseValid != b.recenterBaseValid) {
        return false;
    }
    if (a.posX != b.posX || a.posY != b.posY || a.posZ != b.posZ ||
        a.oriX != b.oriX || a.oriY != b.oriY || a.oriZ != b.oriZ || a.oriW != b.oriW) {
        return false;
    }
    if (!a.recenterBaseValid) return true;
    return a.recenterBase.position.x == b.recenterBase.position.x &&
           a.recenterBase.position.y == b.recenterBase.position.y &&
           a.recenterBase.position.z == b.recenterBase.position.z &&
           a.recenterBase.orientation.x == b.recenterBase.orientation.x &&
           a.recenterBase.orientation.y == b.recenterBase.orientation.y &&
           a.recenterBase.orientation.z == b.recenterBase.orientation.z &&
           a.recenterBase.orientation.w == b.recenterBase.orientation.w;
}
}  // namespace

void cvr::camera::CameraDirectorBlendScopeBegin(const uintptr_t* cameraObjects,
                                                uint32_t capturedCount, uint32_t activeCount,
                                                const OpenXRHeadPose* preferredHead) {
    g_cameraDirectorBlendScope = {};
    if (!cameraObjects || activeCount == 0 || capturedCount != activeCount || capturedCount > 8) {
        return;
    }
    g_cameraDirectorBlendScope.count = capturedCount;
    g_cameraDirectorBlendScope.active = true;
    for (uint32_t i = 0; i < capturedCount; ++i) {
        g_cameraDirectorBlendScope.entries[i].cameraObject = cameraObjects[i];
    }
    if (preferredHead && preferredHead->valid) {
        g_cameraDirectorBlendScope.preferredHead = *preferredHead;
        g_cameraDirectorBlendScope.preferredHeadValid = true;
    }
}

void cvr::camera::CameraDirectorBlendScopeEnd() {
    g_cameraDirectorBlendScope = {};
}

bool cvr::camera::CameraDirectorBlendScopeContains(uintptr_t cameraObject) {
    if (!g_cameraDirectorBlendScope.active || cameraObject < 0x10000) return false;
    for (uint32_t i = 0; i < g_cameraDirectorBlendScope.count; ++i) {
        if (g_cameraDirectorBlendScope.entries[i].cameraObject == cameraObject) return true;
    }
    return false;
}

bool cvr::camera::CameraDirectorBlendScopeReadHead(uintptr_t cameraObject, OpenXRHeadPose* outHead) {
    if (!outHead || !CameraDirectorBlendScopeContains(cameraObject) ||
        !g_cameraDirectorBlendScope.preferredHeadValid) {
        return false;
    }
    *outHead = g_cameraDirectorBlendScope.preferredHead;
    return outHead->valid;
}

bool cvr::camera::CameraDirectorBlendScopeMarkComposed(uintptr_t cameraObject,
                                                       const OpenXRHeadPose& head) {
    if (!head.valid || !g_cameraDirectorBlendScope.active || cameraObject < 0x10000) return false;
    for (uint32_t i = 0; i < g_cameraDirectorBlendScope.count; ++i) {
        CameraDirectorBlendScopeEntry& entry = g_cameraDirectorBlendScope.entries[i];
        if (entry.cameraObject != cameraObject) continue;
        if (entry.composed && !SameHeadSample(entry.head, head)) return false;
        entry.head = head;
        entry.composed = true;
        if (!g_cameraDirectorBlendScope.preferredHeadValid) {
            g_cameraDirectorBlendScope.preferredHead = head;
            g_cameraDirectorBlendScope.preferredHeadValid = true;
        }
        return true;
    }
    return false;
}

bool cvr::camera::CameraDirectorBlendScopeAllComposed(OpenXRHeadPose* outHead) {
    if (!outHead || !g_cameraDirectorBlendScope.active || g_cameraDirectorBlendScope.count == 0) {
        return false;
    }
    const CameraDirectorBlendScopeEntry& first = g_cameraDirectorBlendScope.entries[0];
    if (!first.composed || !first.head.valid) return false;
    for (uint32_t i = 1; i < g_cameraDirectorBlendScope.count; ++i) {
        const CameraDirectorBlendScopeEntry& entry = g_cameraDirectorBlendScope.entries[i];
        if (!entry.composed || !SameHeadSample(first.head, entry.head)) return false;
    }
    *outHead = first.head;
    return true;
}

// Frames identified by a BIT-FOR-BIT match (the normal path) versus by nearest-neighbour (the
// fallback). Measured over a session: exact tracked match one for one, i.e. the engine hands the
// quaternion to the render camera verbatim.
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugFinalExact  = 0;

extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugFinalApprox = 0;

// More than one ring entry was bit-identical. Only possible if two locates returned the very same
// quaternion, which needs a frozen tracker; kept because "impossible" is not a measurement.
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugFinalExactTies = 0;

// The same three numbers for the SECOND view, kept apart from MAIN's so the two can be compared: if
// vrcamMatch tracks match one for one the eyes are being drawn from the same frames, and if it does not,
// the gap IS the second eye's lag -- which was previously invisible because the eye borrowed MAIN's label.
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugVrcamFinalMatch   = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugVrcamFinalNoMatch = 0;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugVrcamFinalAge     = 0;


// EXACT FIRST; NEAREST ONLY AS A FALLBACK THAT ANNOUNCES ITSELF.
//
// Measured over a session: the bit-identical match fires on every single frame -- `exact` tracked
// `match` one for one across thousands of frames. So the engine passes our quaternion to the
// render camera verbatim, and identification is unique BY CONSTRUCTION: two independent
// xrLocateSpace results do not come out bit-identical while a tracker is live, so at most one ring
// entry can match. No threshold, no nearest-search, nothing to tune -- which is the formal 100%
// the approximate path could only approach.
//
// The nearest path stays underneath for the day a game patch changes that, and it is counted
// separately so the change shows up as a number instead of as a symptom. Everything below about
// tolerances applies only to that fallback.
//
// TAKE THE NEAREST ENTRY, MEASURED PROPERLY. Not the first inside a tolerance, and not an
// ordinal pick either -- both of those were wrong, in opposite directions.
//
// First attempt: newest entry with `dot > 0.999999`. That tolerance is 0.16 degrees, so while the
// head barely moves several consecutive writes fall inside it and the scan always returned the
// LAST of them -- a pose from after the one in the frame. Random 0..0.16 deg, about six pixels,
// present or absent per frame: shimmer, and only on micro-movements, because an ordinary turn
// moves further than the tolerance between writes and the match is unique again.
//
// Second attempt: among the candidates, the oldest with `id >= lastMatched`. That freezes. With
// the head still, the previously matched entry keeps satisfying the tolerance, so it is chosen
// again and again while the head quietly drifts; the label stops advancing, the compositor
// reprojects by the whole accumulated difference, and the world slides away under you. The
// reported "floating while holding still" is exactly that.
//
// The real fix is to make the comparison precise enough that there is nothing to disambiguate.
// Two things were in the way:
//
//   * `1 - dot` cancels catastrophically. Head-still tracker noise is on the order of 0.01 deg,
//     which is dot = 1 - 4e-9 -- below float32 resolution, so every candidate compared EQUAL to
//     1.0f and the real nearest one was invisible. Comparing the component difference instead is
//     well conditioned at zero, and in double it resolves far below the noise floor.
//   * The gate still has to be loose, because the engine may renormalise the quaternion between
//     the placed component and the render camera, so a bitwise compare would find nothing at all.
//     Loose gate, precise pick: the gate only rejects nonsense, the minimum decides.
//
// Renormalisation perturbs a component by ~1e-7; the difference between two consecutive writes,
// even with the head still, is ~1e-4. Three orders of magnitude apart, so the nearest entry is
// the written one, unambiguously. No ordering state, nothing to freeze.
bool cvr::camera::CamWriteRecordFind(const float q[4], OpenXRHeadPose* out,
                               uint32_t* outAge, uint32_t* outTies) {
    const uint64_t head = g_camWriteRingHead.load(std::memory_order_acquire);
    const uint64_t n = head < kCamWriteRing ? head : kCamWriteRing;

    bool haveBest = false;
    double bestD = 0.0;
    uint64_t bestId = 0;
    OpenXRHeadPose bestPose{};
    uint32_t ties = 0;

    // IS THE MATCH ACTUALLY EXACT? -- the measurement that decides whether the tolerance is
    // needed at all.
    //
    // The whole reason identification is approximate is the assumption that the engine may
    // renormalise the quaternion between the placed component and the render camera. It probably
    // does somewhere -- sub_1401DA684, a callee of the view-matrix writer, visibly divides by
    // the norm before building a basis -- but that is on the path to the MATRIX, and says nothing
    // about the quaternion field we read. If the field is a straight copy, every match is
    // bit-identical, the tolerance is dead weight, and we can switch to exact compare: unique by
    // construction, no threshold, no nearest-search, formally 100%.
    //
    // So count it. exact == match over a session means the assumption was unnecessary.
    uint32_t exactCount = 0;
    bool haveExact = false;
    uint64_t exactId = 0;
    OpenXRHeadPose exactPose{};

    for (uint64_t i = 1; i <= n; ++i) {
        const CamWriteRecord& r = g_camWriteRing[(head - i) % kCamWriteRing];
        if (!r.valid) continue;
        if (r.quat[0] == q[0] && r.quat[1] == q[1] &&
            r.quat[2] == q[2] && r.quat[3] == q[3]) {
            ++exactCount;
            if (!haveExact) { haveExact = true; exactId = r.id; exactPose = r.pose; }
        }
        double dot = static_cast<double>(r.quat[0]) * q[0] + static_cast<double>(r.quat[1]) * q[1] +
                     static_cast<double>(r.quat[2]) * q[2] + static_cast<double>(r.quat[3]) * q[3];
        // q and -q are the same rotation; align before differencing.
        const double s = dot < 0.0 ? -1.0 : 1.0;
        if (dot * s <= 0.999999) continue;          // gate: obviously not this one
        ++ties;
        double d = 0.0;
        for (int k = 0; k < 4; ++k) {
            const double e = s * static_cast<double>(r.quat[k]) - static_cast<double>(q[k]);
            d += e * e;
        }
        if (!haveBest || d < bestD) {
            bestD = d; bestId = r.id; bestPose = r.pose; haveBest = true;
        }
    }
    // The exact hit decides whenever there is one -- it is the identification, not an estimate.
    if (haveExact) {
        ++CyberpunkVR_DebugFinalExact;
        if (exactCount > 1) ++CyberpunkVR_DebugFinalExactTies;
        if (out)     *out = exactPose;
        if (outAge)  *outAge = static_cast<uint32_t>((head - 1) - exactId);
        if (outTies) *outTies = exactCount;
        return true;
    }
    if (!haveBest) return false;

    // Nearest-neighbour fallback. SAY SO -- a silent degrade here is a symptom with no cause, and
    // that is the whole reason this hunt took as long as it did. RealVR does the same thing at the
    // equivalent point ("Rendering pose entry is invalid").
    ++CyberpunkVR_DebugFinalApprox;
    {
        static uint64_t s_lastReport = 0;
        if (CyberpunkVR_DebugFinalApprox - s_lastReport >= 600) {
            s_lastReport = CyberpunkVR_DebugFinalApprox;
            Log("POSEDIAG: WARNING -- the render camera quaternion is no longer a verbatim copy "
                "(approx=%llu exact=%llu). Frame identification has degraded to nearest-neighbour; "
                "expect the pose label to be within %.3f deg rather than exact.\n",
                (unsigned long long)CyberpunkVR_DebugFinalApprox,
                (unsigned long long)CyberpunkVR_DebugFinalExact,
                0.16);
        }
    }
    if (out)     *out = bestPose;
    if (outAge)  *outAge = static_cast<uint32_t>((head - 1) - bestId);
    if (outTies) *outTies = ties;
    return true;
}

// ---- the view frame handed to the solve ---------------------------------------------------------
//
// A seqlock, for the same reason the write quaternion is one: the producer runs on the engine's
// render/job thread and the consumer inside the animation pass, and a reader that catches half a
// publication gets a frame of reference that existed at no instant. Odd = write in progress, and
// the reader retries; even and unchanged across the read = the copy is whole.
namespace {
std::atomic<uint32_t> g_lcfSeq{0};
struct AtomicLocatedCameraFrame {
    std::atomic<float> worldPos[3]{};
    std::atomic<float> worldQuat[4]{};
    std::atomic<uint32_t> sequence{0};
    std::atomic<uint32_t> frameEpoch{0};
};
AtomicLocatedCameraFrame g_lcf{};
std::atomic<uint32_t>  g_vfSeq{0};
cvr::camera::ViewFrame g_vf{};
// The barrel packet gets its own sequence: it is published from the final-camera callback, the view
// frame from the locate solve, and sharing one counter would make each writer's readers retry on the
// other's traffic for no reason.
std::atomic<uint32_t>    g_bfSeq{0};
cvr::camera::BarrelFrame g_bf{};
}  // namespace

namespace cvr::camera {

void LocatedCameraFramePublish(const LocatedCameraFrame& f) {
    g_lcfSeq.fetch_add(1u, std::memory_order_acq_rel);  // odd
    for (int i = 0; i < 3; ++i) {
        g_lcf.worldPos[i].store(f.worldPos[i], std::memory_order_relaxed);
    }
    for (int i = 0; i < 4; ++i) {
        g_lcf.worldQuat[i].store(f.worldQuat[i], std::memory_order_relaxed);
    }
    g_lcf.sequence.store(f.sequence, std::memory_order_relaxed);
    g_lcf.frameEpoch.store(f.frameEpoch, std::memory_order_relaxed);
    g_lcfSeq.fetch_add(1u, std::memory_order_release);  // even
}

bool LocatedCameraFrameRead(LocatedCameraFrame* out) {
    if (!out) return false;
    for (int tries = 0; tries < 8; ++tries) {
        const uint32_t s0 = g_lcfSeq.load(std::memory_order_acquire);
        if (s0 == 0u) return false;
        if (s0 & 1u) continue;
        LocatedCameraFrame tmp{};
        for (int i = 0; i < 3; ++i) {
            tmp.worldPos[i] = g_lcf.worldPos[i].load(std::memory_order_relaxed);
        }
        for (int i = 0; i < 4; ++i) {
            tmp.worldQuat[i] = g_lcf.worldQuat[i].load(std::memory_order_relaxed);
        }
        tmp.sequence = g_lcf.sequence.load(std::memory_order_relaxed);
        tmp.frameEpoch = g_lcf.frameEpoch.load(std::memory_order_relaxed);
        if (g_lcfSeq.load(std::memory_order_acquire) == s0) {
            *out = tmp;
            return true;
        }
    }
    return false;
}

void ViewFramePublish(const ViewFrame& f) {
    const uint32_t s = g_vfSeq.load(std::memory_order_relaxed) + 1u;   // odd
    g_vfSeq.store(s, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    g_vf = f;
    std::atomic_thread_fence(std::memory_order_release);
    g_vfSeq.store(s + 1u, std::memory_order_relaxed);                  // even = complete
}

void BarrelFramePublish(const BarrelFrame& f) {
    const uint32_t s = g_bfSeq.load(std::memory_order_relaxed) + 1u;   // odd
    g_bfSeq.store(s, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    g_bf = f;
    std::atomic_thread_fence(std::memory_order_release);
    g_bfSeq.store(s + 1u, std::memory_order_relaxed);                  // even = complete
}

bool BarrelFrameRead(BarrelFrame* out) {
    if (!out) return false;
    for (int tries = 0; tries < 8; ++tries) {
        const uint32_t s0 = g_bfSeq.load(std::memory_order_relaxed);
        if (s0 == 0u) return false;          // nothing published yet
        if (s0 & 1u) continue;               // write in progress
        std::atomic_thread_fence(std::memory_order_acquire);
        const BarrelFrame tmp = g_bf;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_bfSeq.load(std::memory_order_relaxed) == s0) {
            *out = tmp;
            return true;
        }
    }
    return false;
}

bool ViewFrameRead(ViewFrame* out) {
    if (!out) return false;
    for (int tries = 0; tries < 8; ++tries) {
        const uint32_t s0 = g_vfSeq.load(std::memory_order_relaxed);
        if (s0 == 0u) return false;          // nothing published yet
        if (s0 & 1u) continue;               // write in progress
        std::atomic_thread_fence(std::memory_order_acquire);
        const ViewFrame tmp = g_vf;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_vfSeq.load(std::memory_order_relaxed) == s0) {
            *out = tmp;
            return true;
        }
    }
    return false;
}

}  // namespace cvr::camera
