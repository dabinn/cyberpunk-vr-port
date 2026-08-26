// Phase 3E: Phase 3D pose handoff applied to whichever RTT component CET selects.
//
// At the selected world VRCAM's sub_140AC31C4 entry, take a double-read snapshot of the MAIN
// placed component. MAIN's E0 is already its submitted eye when CyberpunkVR_IpdInWorldPos is on,
// so move one full IPD to the opposite eye; if that path is off, treat E0 as center and move only
// the VRCAM half-IPD. Temporarily publish that E0/F0 through the native transform-changed doorbell,
// then restore the fixed world VRCAM source before normal refresh. The owner and component remain
// fixed; only the RTT producer's persistent camera state changes.

#include "Camera/CameraLink.hpp"
#include "Camera/CameraState.hpp"
#include "Runtimes/OpenXRManager.hpp"
#include "Stereo/DetourRegistry.hpp"
#include "Stereo/EngineRvas.hpp"
#include "Stereo/StereoInternal.hpp"
#include "Utils/MemorySafe.hpp"
#include "Utils/StereoLog.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace cvr {
namespace detail {

using RttTransformChangedFn = void (__fastcall*)(uintptr_t component, uintptr_t changePacket);
using RttCameraRefreshFn = void (__fastcall*)(uintptr_t component);

struct RawPlacedPose {
    uint32_t position[3]{};
    uint32_t quaternion[4]{};
};

static RttCameraRefreshFn g_orig_rtt_camera_refresh = nullptr;
static std::atomic<uint64_t> g_boundHits{0};
static std::atomic<uint64_t> g_stableAccepted{0};
static std::atomic<uint64_t> g_stableRejected{0};
static std::atomic<uint64_t> g_publishCalls{0};
static std::atomic<uint64_t> g_publishFailures{0};
static std::atomic<uint64_t> g_restoreFailures{0};
static std::atomic<uint64_t> g_eyeOffsetFailures{0};
static std::atomic<uint64_t> g_trueMainPublishes{0};
static std::atomic<uint64_t> g_sourceHandoffs{0};

static std::atomic<uintptr_t> g_sourceStateComponent{0};
static std::atomic<bool> g_useTrueMain{false};

struct MainSourceSelection {
    RawPlacedPose pose{};
    bool useTrueMain = false;
    bool finalMainValid = false;
    uint32_t finalSequence = 0;
    uint32_t finalLocateSequence = 0;
    int64_t finalAgeUs = -1;
    float positionGap = 0.0f;
    float orientationGapDeg = 0.0f;
};

static bool ReadU32VolatileSafe(uintptr_t address, uint32_t* out) {
    __try {
        // Volatile is intentional: the two stable-snapshot passes must issue two observable loads
        // from engine-owned memory. A plain inline ReadU32Safe load could legally be commoned by
        // the optimizer because this function has no C++ ownership relationship with the writer.
        *out = *reinterpret_cast<volatile const uint32_t*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool ReadRawPose(uintptr_t component, RawPlacedPose* out) {
    if (!component || !out) return false;
    RawPlacedPose value{};
    for (int i = 0; i < 3; ++i) {
        if (!ReadU32VolatileSafe(component + 0xE0 + sizeof(uint32_t) * i, &value.position[i])) return false;
    }
    for (int i = 0; i < 4; ++i) {
        if (!ReadU32VolatileSafe(component + 0xF0 + sizeof(uint32_t) * i, &value.quaternion[i])) return false;
    }
    *out = value;
    return true;
}

static bool RawPoseEqual(const RawPlacedPose& a, const RawPlacedPose& b) {
    for (int i = 0; i < 3; ++i) {
        if (a.position[i] != b.position[i]) return false;
    }
    for (int i = 0; i < 4; ++i) {
        if (a.quaternion[i] != b.quaternion[i]) return false;
    }
    return true;
}

static bool IsPlausibleRawPose(const RawPlacedPose& pose) {
    float q[4]{};
    std::memcpy(q, pose.quaternion, sizeof(q));
    return IsPlausibleUnitQuaternion(q);
}

static bool ReadStableMainPose(uintptr_t component, RawPlacedPose* out) {
    if (!component || !out) return false;
    RawPlacedPose first{};
    RawPlacedPose second{};
    if (!ReadRawPose(component, &first) || !ReadRawPose(component, &second)) return false;
    if (!RawPoseEqual(first, second) || !IsPlausibleRawPose(second)) return false;
    *out = second;
    return true;
}

static bool WriteRawPose(uintptr_t component, const RawPlacedPose& pose) {
    if (!component) return false;
    for (int i = 0; i < 3; ++i) {
        if (!WriteU32Safe(component + 0xE0 + sizeof(uint32_t) * i, pose.position[i])) return false;
    }
    for (int i = 0; i < 4; ++i) {
        if (!WriteU32Safe(component + 0xF0 + sizeof(uint32_t) * i, pose.quaternion[i])) return false;
    }
    return true;
}

static void DecodePose(const RawPlacedPose& raw, float outPos[3], float outQuat[4]) {
    for (int i = 0; i < 3; ++i) {
        outPos[i] = static_cast<float>(static_cast<int32_t>(raw.position[i])) / 131072.0f;
    }
    std::memcpy(outQuat, raw.quaternion, sizeof(float) * 4);
}

static bool EncodePose(const float pos[3], const float quat[4], RawPlacedPose* out) {
    if (!out || !IsPlausibleUnitQuaternion(quat)) return false;
    RawPlacedPose raw{};
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(pos[i])) return false;
        const double fixed = std::round(static_cast<double>(pos[i]) * 131072.0);
        if (fixed < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
            fixed > static_cast<double>(std::numeric_limits<int32_t>::max())) {
            return false;
        }
        raw.position[i] = static_cast<uint32_t>(static_cast<int32_t>(fixed));
    }
    std::memcpy(raw.quaternion, quat, sizeof(raw.quaternion));
    *out = raw;
    return true;
}

static float QuaternionGapDegrees(const float a[4], const float b[4]) {
    double dot = 0.0;
    for (int i = 0; i < 4; ++i) dot += static_cast<double>(a[i]) * b[i];
    dot = std::clamp(std::abs(dot), 0.0, 1.0);
    return static_cast<float>(2.0 * std::acos(dot) * (180.0 / 3.14159265358979323846));
}

static MainSourceSelection SelectMainSource(uintptr_t worldComponent,
                                            const RawPlacedPose& currentFpp) {
    MainSourceSelection selected{};
    selected.pose = currentFpp;

    cvr::camera::FinalMainCameraFrame finalMain{};
    const uint64_t nowUs = XrDiagNowUs();
    selected.finalMainValid = cvr::camera::FinalMainCameraFrameRead(&finalMain) &&
                              finalMain.sequence != 0 &&
                              IsPlausibleUnitQuaternion(finalMain.worldQuat) &&
                              nowUs >= finalMain.timestampUs &&
                              (nowUs - finalMain.timestampUs) <= 100000u;
    if (selected.finalMainValid) {
        selected.finalSequence = finalMain.sequence;
        selected.finalLocateSequence = finalMain.locateSequence;
        selected.finalAgeUs = static_cast<int64_t>(nowUs - finalMain.timestampUs);

        float fppPos[3]{};
        float fppQuat[4]{};
        DecodePose(currentFpp, fppPos, fppQuat);
        const float dx = finalMain.worldPos[0] - fppPos[0];
        const float dy = finalMain.worldPos[1] - fppPos[1];
        const float dz = finalMain.worldPos[2] - fppPos[2];
        selected.positionGap = std::sqrt(dx * dx + dy * dy + dz * dz);
        selected.orientationGapDeg = QuaternionGapDegrees(finalMain.worldQuat, fppQuat);
    }

    if (g_sourceStateComponent.exchange(worldComponent, std::memory_order_acq_rel) != worldComponent) {
        g_useTrueMain.store(false, std::memory_order_release);
    }

    const bool wasTrueMain = g_useTrueMain.load(std::memory_order_acquire);
    bool useTrueMain = wasTrueMain;
    if (!selected.finalMainValid) {
        useTrueMain = false;
    } else if (!useTrueMain) {
        // Current-vs-last-MAIN timing measured up to 8.67 mm / 2.22 degrees in FPP. These entry
        // thresholds intentionally sit well above that normal inter-view age while still catching
        // vehicle orbit and scene cameras whose origin or heading separates from the FPP camera.
        useTrueMain = selected.positionGap >= 0.75f || selected.orientationGapDeg >= 20.0f;
    } else {
        // Hysteresis keeps camera blends from alternating the source on adjacent RTT refreshes.
        useTrueMain = selected.positionGap >= 0.20f || selected.orientationGapDeg >= 6.0f;
    }
    g_useTrueMain.store(useTrueMain, std::memory_order_release);
    if (wasTrueMain != useTrueMain) {
        g_sourceHandoffs.fetch_add(1u, std::memory_order_relaxed);
    }

    if (useTrueMain) {
        RawPlacedPose trueMain{};
        if (EncodePose(finalMain.worldPos, finalMain.worldQuat, &trueMain)) {
            selected.pose = trueMain;
            selected.useTrueMain = true;
        } else {
            g_useTrueMain.store(false, std::memory_order_release);
        }
    }
    return selected;
}

static bool BuildVrcamEyePose(const RawPlacedPose& mainPose, RawPlacedPose* out,
                              float* outSignedOffset) {
    if (!out) return false;

    float q[4]{};
    std::memcpy(q, mainPose.quaternion, sizeof(q));
    if (!IsPlausibleUnitQuaternion(q)) return false;

    float right[3]{};
    ComputeRightVectorFromQuaternion(q, right);
    if (!IsPlausibleUnitVector3(right)) return false;

    const float halfIpd = GetDesiredHalfIpd();
    if (!(halfIpd > 0.0001f) || !std::isfinite(halfIpd)) return false;

    // OpenXR eye 0 is LEFT and eye 1 is RIGHT. MainIsRightEye=1 sends MAIN to RIGHT and VRCAM
    // to LEFT, so the VRCAM lies in the negative camera-right direction from MAIN. With the
    // mapping reversed the sign reverses too. If MAIN's E0 already has its own half-IPD, the
    // distance between the two eye viewpoints is one full IPD; otherwise E0 is center.
    const float sign = CyberpunkVR_MainIsRightEye ? -1.0f : 1.0f;
    const float signedOffset = sign * halfIpd * (CyberpunkVR_IpdInWorldPos ? 2.0f : 1.0f);

    RawPlacedPose result = mainPose;
    for (int i = 0; i < 3; ++i) {
        const int64_t source = static_cast<int32_t>(mainPose.position[i]);
        const int64_t delta = static_cast<int64_t>(std::llround(
            right[i] * signedOffset * 131072.0f));
        const int64_t shifted = source + delta;
        if (shifted < std::numeric_limits<int32_t>::min() ||
            shifted > std::numeric_limits<int32_t>::max()) {
            return false;
        }
        result.position[i] = static_cast<uint32_t>(static_cast<int32_t>(shifted));
    }

    *out = result;
    if (outSignedOffset) *outSignedOffset = signedOffset;
    return true;
}

static uintptr_t ReadNestedCamera(uintptr_t component) {
    uintptr_t owner = 0;
    uintptr_t resource = 0;
    uintptr_t stateHolder = 0;
    uintptr_t state = 0;
    uintptr_t camera = 0;
    if (!ReadPtrSafe(component + 0x50, &owner) || !owner) return 0;
    if (!ReadPtrSafe(owner + 0xB8, &resource) || !resource) return 0;
    if (!ReadPtrSafe(resource + 0x08, &stateHolder) || !stateHolder) return 0;
    if (!ReadPtrSafe(stateHolder, &state) || !state) return 0;
    if (!ReadPtrSafe(state + 0xA0, &camera)) return 0;
    return camera;
}

static RttTransformChangedFn GetTransformChangedCallback(uintptr_t component) {
    uintptr_t vtable = 0;
    uintptr_t callback = 0;
    if (!ReadPtrSafe(component, &vtable) || !vtable) return nullptr;
    if (!ReadPtrSafe(vtable + 0x240, &callback) || !callback) return nullptr;
    return reinterpret_cast<RttTransformChangedFn>(callback);
}

static void ReportPublish(uintptr_t worldComponent, uintptr_t mainComponent,
                          const MainSourceSelection& selection,
                          const RawPlacedPose& publishedPose,
                          float signedEyeOffset, uintptr_t nestedBefore,
                          uintptr_t nestedAfter, bool restoredExactly) {
    float sourcePos[3]{};
    float sourceQuat[4]{};
    float publishedPos[3]{};
    float publishedQuat[4]{};
    DecodePose(selection.pose, sourcePos, sourceQuat);
    DecodePose(publishedPose, publishedPos, publishedQuat);

    Log("[worldvrcam-3D] hits=%llu stableAccepted=%llu stableRejected=%llu publishes=%llu "
        "publishFailures=%llu restoreFailures=%llu "
        "worldComp=%p mainComp=%p nestedBefore=%p nestedAfter=%p restored=%d "
        "source=%s handoffs=%llu trueMainPublishes=%llu finalValid=%d "
        "finalSeq=%u finalLocateSeq=%u finalAgeUs=%lld gapM=%.4f gapDeg=%.3f "
        "mainP=(%.5f,%.5f,%.5f) vrcamP=(%.5f,%.5f,%.5f) "
        "eyeOffset=%.5f mainIsRight=%d ipdInWorld=%d eyeOffsetFailures=%llu "
        "sourceQ=(%.6f,%.6f,%.6f,%.6f) "
        "finalOk=%d finalSeq=%u finalLocateSeq=%u finalAgeUs=%lld\n",
        static_cast<unsigned long long>(g_boundHits.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_stableAccepted.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_stableRejected.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_publishCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_publishFailures.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_restoreFailures.load(std::memory_order_relaxed)),
        reinterpret_cast<void*>(worldComponent), reinterpret_cast<void*>(mainComponent),
        reinterpret_cast<void*>(nestedBefore), reinterpret_cast<void*>(nestedAfter),
        restoredExactly ? 1 : 0,
        selection.useTrueMain ? "true-main" : "fpp-current",
        static_cast<unsigned long long>(g_sourceHandoffs.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_trueMainPublishes.load(std::memory_order_relaxed)),
        selection.finalMainValid ? 1 : 0, selection.finalSequence,
        selection.finalLocateSequence, static_cast<long long>(selection.finalAgeUs),
        selection.positionGap, selection.orientationGapDeg,
        sourcePos[0], sourcePos[1], sourcePos[2],
        publishedPos[0], publishedPos[1], publishedPos[2],
        signedEyeOffset, CyberpunkVR_MainIsRightEye ? 1 : 0,
        CyberpunkVR_IpdInWorldPos ? 1 : 0,
        static_cast<unsigned long long>(g_eyeOffsetFailures.load(std::memory_order_relaxed)),
        sourceQuat[0], sourceQuat[1], sourceQuat[2], sourceQuat[3]);
}

static void PublishCurrentMainPose(uintptr_t worldComponent) {
    const uintptr_t mainComponent = g_camObjMain.load(std::memory_order_acquire);
    RawPlacedPose source{};
    if (!ReadStableMainPose(mainComponent, &source)) {
        g_stableRejected.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    g_stableAccepted.fetch_add(1u, std::memory_order_relaxed);

    const MainSourceSelection selection = SelectMainSource(worldComponent, source);
    source = selection.pose;
    if (selection.useTrueMain) {
        g_trueMainPublishes.fetch_add(1u, std::memory_order_relaxed);
    }

    RawPlacedPose vrcamPose{};
    float signedEyeOffset = 0.0f;
    if (!BuildVrcamEyePose(source, &vrcamPose, &signedEyeOffset)) {
        g_eyeOffsetFailures.fetch_add(1u, std::memory_order_relaxed);
        return;
    }

    RawPlacedPose original{};
    if (!ReadRawPose(worldComponent, &original)) {
        g_publishFailures.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    const auto transformChanged = GetTransformChangedCallback(worldComponent);
    if (!transformChanged) {
        g_publishFailures.fetch_add(1u, std::memory_order_relaxed);
        return;
    }

    const uintptr_t nestedBefore = ReadNestedCamera(worldComponent);
    bool wroteTemporary = false;
    bool callbackCompleted = false;
    __try {
        wroteTemporary = WriteRawPose(worldComponent, vrcamPose);
        if (wroteTemporary) {
            transformChanged(worldComponent, worldComponent + 0x100);
            callbackCompleted = true;
            g_publishCalls.fetch_add(1u, std::memory_order_relaxed);
        }
    }
    __finally {
        if (wroteTemporary) WriteRawPose(worldComponent, original);
    }

    RawPlacedPose restored{};
    const bool restoredExactly = ReadRawPose(worldComponent, &restored) &&
                                 RawPoseEqual(original, restored);
    if (!restoredExactly) g_restoreFailures.fetch_add(1u, std::memory_order_relaxed);
    if (!callbackCompleted) {
        g_publishFailures.fetch_add(1u, std::memory_order_relaxed);
        return;
    }

    const uint64_t published = g_publishCalls.load(std::memory_order_relaxed);
    if ((published % 120u) == 1u || !restoredExactly) {
        ReportPublish(worldComponent, mainComponent, selection, vrcamPose, signedEyeOffset, nestedBefore,
                      ReadNestedCamera(worldComponent), restoredExactly);
    }
}

static void __fastcall Detour_RttCameraRefresh(uintptr_t component) {
    const uintptr_t boundComponent = g_vrcam_comp.load(std::memory_order_acquire);
    if (component && component == boundComponent) {
        g_boundHits.fetch_add(1u, std::memory_order_relaxed);
        PublishCurrentMainPose(component);
    }
    g_orig_rtt_camera_refresh(component);
}

CVR_DETOUR("[vrcam-test] RTT camera refresh Phase 3E selected-owner MAIN handoff",
           RTT_CAMERA_REFRESH_RVA,
           Detour_RttCameraRefresh,
           g_orig_rtt_camera_refresh)

}  // namespace detail
}  // namespace cvr
