// Generic active-camera handoff for the selected RTT VRCAM.
//
// The player-owned VRCAM remains upstream's component. When dispatcher MAIN is clearly detached
// from the player's FPP camera, temporarily publish that rendered MAIN pose into the VRCAM at its
// native RTT refresh boundary, move it to the opposite eye, notify the engine, then restore the
// component exactly. FPP is deliberately untouched so the 0.1.6 second-view path keeps ownership.

#include "Camera/CameraLink.hpp"
#include "Camera/CameraState.hpp"
#include "Core/LiveControls.hpp"
#include "Stereo/DetourRegistry.hpp"
#include "Stereo/EngineRvas.hpp"
#include "Stereo/StereoInternal.hpp"
#include "Utils/MemorySafe.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace cvr::detail {
namespace {

using RttTransformChangedFn = void (__fastcall*)(uintptr_t component, uintptr_t changePacket);
using RttCameraRefreshFn = void (__fastcall*)(uintptr_t component);

struct RawPlacedPose {
    uint32_t position[3]{};
    uint32_t quaternion[4]{};
};

struct MainSourceSelection {
    RawPlacedPose pose{};
    bool useTrueMain = false;
    bool hmdComposed = false;
    OpenXRHeadPose hmdPose{};
    bool directorMainValid = false;
    bool finalMainValid = false;
    float positionGap = 0.0f;
    float orientationGapDeg = 0.0f;
};

RttCameraRefreshFn g_origRttCameraRefresh = nullptr;
std::atomic<uintptr_t> g_sourceStateComponent{0};
std::atomic<bool> g_useTrueMain{false};

bool ReadU32VolatileSafe(uintptr_t address, uint32_t* out) {
    __try {
        *out = *reinterpret_cast<volatile const uint32_t*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadRawPose(uintptr_t component, RawPlacedPose* out) {
    if (!component || !out) return false;
    RawPlacedPose value{};
    for (int i = 0; i < 3; ++i) {
        if (!ReadU32VolatileSafe(component + 0xE0u + sizeof(uint32_t) * i, &value.position[i])) return false;
    }
    for (int i = 0; i < 4; ++i) {
        if (!ReadU32VolatileSafe(component + 0xF0u + sizeof(uint32_t) * i, &value.quaternion[i])) return false;
    }
    *out = value;
    return true;
}

bool RawPoseEqual(const RawPlacedPose& a, const RawPlacedPose& b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

bool ReadStablePose(uintptr_t component, RawPlacedPose* out) {
    RawPlacedPose first{};
    RawPlacedPose second{};
    if (!ReadRawPose(component, &first) || !ReadRawPose(component, &second)) return false;
    if (!RawPoseEqual(first, second)) return false;
    float q[4]{};
    std::memcpy(q, second.quaternion, sizeof(q));
    if (!IsPlausibleUnitQuaternion(q)) return false;
    *out = second;
    return true;
}

bool WriteRawPose(uintptr_t component, const RawPlacedPose& pose) {
    if (!component) return false;
    for (int i = 0; i < 3; ++i) {
        if (!WriteU32Safe(component + 0xE0u + sizeof(uint32_t) * i, pose.position[i])) return false;
    }
    for (int i = 0; i < 4; ++i) {
        if (!WriteU32Safe(component + 0xF0u + sizeof(uint32_t) * i, pose.quaternion[i])) return false;
    }
    return true;
}

void DecodePose(const RawPlacedPose& raw, float outPos[3], float outQuat[4]) {
    for (int i = 0; i < 3; ++i) {
        outPos[i] = static_cast<float>(static_cast<int32_t>(raw.position[i])) / 131072.0f;
    }
    std::memcpy(outQuat, raw.quaternion, sizeof(float) * 4);
}

bool EncodePose(const float pos[3], const float quat[4], RawPlacedPose* out) {
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

float QuaternionGapDegrees(const float a[4], const float b[4]) {
    double dot = 0.0;
    for (int i = 0; i < 4; ++i) dot += static_cast<double>(a[i]) * b[i];
    dot = std::clamp(std::abs(dot), 0.0, 1.0);
    return static_cast<float>(2.0 * std::acos(dot) * (180.0 / 3.14159265358979323846));
}

MainSourceSelection SelectMainSource(uintptr_t vrcamComponent, const RawPlacedPose& currentFpp) {
    MainSourceSelection selected{};
    selected.pose = currentFpp;

    const uint64_t nowUs = XrDiagNowUs();
    float fppPos[3]{};
    float fppQuat[4]{};
    DecodePose(currentFpp, fppPos, fppQuat);

    cvr::camera::GenericNonFppCameraFrame currentDirector{};
    const cvr::camera::GenericNonFppCurrentBlendState currentBlendState =
        cvr::camera::GenericNonFppCurrentBlendRead(&currentDirector);
    const bool currentDirectorValid =
        currentBlendState == cvr::camera::GenericNonFppCurrentBlendState::Ready &&
        currentDirector.sequence != 0 && currentDirector.active != 0 &&
        IsPlausibleUnitQuaternion(currentDirector.worldQuat);

    cvr::camera::GenericNonFppCameraFrame directorMain{};
    const bool completedDirectorValid =
        currentBlendState != cvr::camera::GenericNonFppCurrentBlendState::NoGeneric &&
        cvr::camera::GenericNonFppCameraFrameRead(&directorMain) &&
        directorMain.sequence != 0 && directorMain.active != 0 &&
        IsPlausibleUnitQuaternion(directorMain.worldQuat) &&
        nowUs >= directorMain.timestampUs &&
        (nowUs - directorMain.timestampUs) <= 50000u;
    selected.directorMainValid = currentDirectorValid || completedDirectorValid;
    if (selected.directorMainValid) {
        const cvr::camera::GenericNonFppCameraFrame& source =
            currentDirectorValid ? currentDirector : directorMain;
        const float dx = source.worldPos[0] - fppPos[0];
        const float dy = source.worldPos[1] - fppPos[1];
        const float dz = source.worldPos[2] - fppPos[2];
        selected.positionGap = std::sqrt(dx * dx + dy * dy + dz * dz);
        selected.orientationGapDeg = QuaternionGapDegrees(source.worldQuat, fppQuat);
    }

    cvr::camera::FinalMainCameraFrame finalMain{};
    selected.finalMainValid = cvr::camera::FinalMainCameraFrameRead(&finalMain) &&
                              finalMain.sequence != 0 &&
                              IsPlausibleUnitQuaternion(finalMain.worldQuat) &&
                              nowUs >= finalMain.timestampUs &&
                              (nowUs - finalMain.timestampUs) <= 100000u;
    if (selected.finalMainValid) {
        if (!selected.directorMainValid) {
            const float dx = finalMain.worldPos[0] - fppPos[0];
            const float dy = finalMain.worldPos[1] - fppPos[1];
            const float dz = finalMain.worldPos[2] - fppPos[2];
            selected.positionGap = std::sqrt(dx * dx + dy * dy + dz * dz);
            selected.orientationGapDeg = QuaternionGapDegrees(finalMain.worldQuat, fppQuat);
        }
    }

    if (g_sourceStateComponent.exchange(vrcamComponent, std::memory_order_acq_rel) != vrcamComponent) {
        g_useTrueMain.store(false, std::memory_order_release);
    }

    const bool wasTrueMain = g_useTrueMain.load(std::memory_order_acquire);
    bool useTrueMain = wasTrueMain;
    if (currentBlendState == cvr::camera::GenericNonFppCurrentBlendState::NoGeneric) {
        useTrueMain = false;
    } else if (selected.directorMainValid) {
        useTrueMain = true;
    } else if (!selected.finalMainValid) {
        useTrueMain = false;
    } else if (!useTrueMain) {
        useTrueMain = selected.positionGap >= 0.75f || selected.orientationGapDeg >= 20.0f;
    } else {
        useTrueMain = selected.positionGap >= 0.20f || selected.orientationGapDeg >= 6.0f;
    }
    g_useTrueMain.store(useTrueMain, std::memory_order_release);

    if (useTrueMain) {
        RawPlacedPose trueMain{};
        const cvr::camera::GenericNonFppCameraFrame* directorSource = currentDirectorValid
            ? &currentDirector
            : (completedDirectorValid ? &directorMain : nullptr);
        const float* sourcePos = directorSource ? directorSource->worldPos : finalMain.worldPos;
        const float* sourceQuat = directorSource ? directorSource->worldQuat : finalMain.worldQuat;
        if (EncodePose(sourcePos, sourceQuat, &trueMain)) {
            selected.pose = trueMain;
            selected.useTrueMain = true;
            if (directorSource) {
                selected.hmdComposed = directorSource->hmdComposed != 0 && directorSource->hmdPose.valid;
                if (selected.hmdComposed) selected.hmdPose = directorSource->hmdPose;
            } else {
                selected.hmdComposed = finalMain.hmdComposed != 0 && finalMain.hmdPose.valid;
                if (selected.hmdComposed) selected.hmdPose = finalMain.hmdPose;
            }
        } else {
            g_useTrueMain.store(false, std::memory_order_release);
        }
    }
    return selected;
}

bool BuildOppositeEyePose(const RawPlacedPose& mainPose, const float* eyeOrientation,
                          RawPlacedPose* out) {
    if (!out) return false;
    float q[4]{};
    if (eyeOrientation) std::memcpy(q, eyeOrientation, sizeof(q));
    else std::memcpy(q, mainPose.quaternion, sizeof(q));
    if (!IsPlausibleUnitQuaternion(q)) return false;

    float right[3]{};
    ComputeRightVectorFromQuaternion(q, right);
    if (!IsPlausibleUnitVector3(right)) return false;

    const float halfIpd = GetDesiredHalfIpd();
    if (!(halfIpd > 0.0001f) || !std::isfinite(halfIpd)) return false;
    const float sign = CyberpunkVR_MainIsRightEye ? -1.0f : 1.0f;
    const float signedOffset = sign * halfIpd * (CyberpunkVR_IpdInWorldPos ? 2.0f : 1.0f);

    RawPlacedPose result = mainPose;
    for (int i = 0; i < 3; ++i) {
        const int64_t source = static_cast<int32_t>(mainPose.position[i]);
        const int64_t delta = static_cast<int64_t>(std::llround(right[i] * signedOffset * 131072.0f));
        const int64_t shifted = source + delta;
        if (shifted < std::numeric_limits<int32_t>::min() || shifted > std::numeric_limits<int32_t>::max()) {
            return false;
        }
        result.position[i] = static_cast<uint32_t>(static_cast<int32_t>(shifted));
    }
    *out = result;
    return true;
}

RttTransformChangedFn GetTransformChangedCallback(uintptr_t component) {
    uintptr_t vtable = 0;
    uintptr_t callback = 0;
    if (!ReadPtrSafe(component, &vtable) || !vtable) return nullptr;
    if (!ReadPtrSafe(vtable + 0x240u, &callback) || !callback) return nullptr;
    return reinterpret_cast<RttTransformChangedFn>(callback);
}

void PublishActiveMainToVrcam(uintptr_t vrcamComponent) {
    const uintptr_t fppComponent = g_camObjMain.load(std::memory_order_acquire);
    RawPlacedPose currentFpp{};
    if (!ReadStablePose(fppComponent, &currentFpp)) return;

    const MainSourceSelection selection = SelectMainSource(vrcamComponent, currentFpp);
    if (!selection.useTrueMain) return;

    // Proven HMD-composed sources carry a clean gameplay-camera quaternion plus the exact HMD
    // sample. Recompose it here so the opposite-eye axis and the temporary VRCAM share that sample.
    float eyeOrientation[4]{};
    const float* eyeOrientationPtr = nullptr;
    OpenXRHeadPose genericHead{};
    if (selection.hmdComposed) {
        float baseQuat[4]{};
        std::memcpy(baseQuat, selection.pose.quaternion, sizeof(baseQuat));
        genericHead = selection.hmdPose;
        if (!IsPlausibleUnitQuaternion(baseQuat) || !genericHead.valid) return;
        MulQuat(baseQuat[0], baseQuat[1], baseQuat[2], baseQuat[3],
                genericHead.oriX, -genericHead.oriZ, genericHead.oriY, genericHead.oriW,
                eyeOrientation[0], eyeOrientation[1], eyeOrientation[2], eyeOrientation[3]);
        NormalizeQuat(eyeOrientation[0], eyeOrientation[1], eyeOrientation[2], eyeOrientation[3]);
        if (!IsPlausibleUnitQuaternion(eyeOrientation)) return;
        eyeOrientationPtr = eyeOrientation;
    }

    RawPlacedPose vrcamPose{};
    if (!BuildOppositeEyePose(selection.pose, eyeOrientationPtr, &vrcamPose)) return;
    if (selection.hmdComposed) {
        std::memcpy(vrcamPose.quaternion, eyeOrientation, sizeof(vrcamPose.quaternion));
    }

    RawPlacedPose original{};
    const auto transformChanged = GetTransformChangedCallback(vrcamComponent);
    if (!ReadRawPose(vrcamComponent, &original) || !transformChanged) return;

    bool writeAttempted = false;
    bool callbackCompleted = false;
    bool locateScopeInstalled = false;
    cvr::camera::GenericVrcamLocateScope previousScope{};
    cvr::camera::GenericVrcamLocateScope completedScope{};
    if (selection.hmdComposed) {
        cvr::camera::GenericVrcamLocateScope scope{};
        scope.cameraObject = vrcamComponent;
        scope.head = genericHead;
        scope.active = true;
        scope.entryAlreadyComposed = true;
        previousScope = cvr::camera::GenericVrcamLocateScopeExchange(scope);
        locateScopeInstalled = true;
    }
    __try {
        writeAttempted = true;
        if (WriteRawPose(vrcamComponent, vrcamPose)) {
            transformChanged(vrcamComponent, vrcamComponent + 0x100u);
            callbackCompleted = true;
        }
    }
    __finally {
        if (locateScopeInstalled) {
            completedScope = cvr::camera::GenericVrcamLocateScopeExchange(previousScope);
        }
        if (writeAttempted) WriteRawPose(vrcamComponent, original);
    }

    if (!callbackCompleted) return;
    if (selection.hmdComposed && !completedScope.locateConsumed) {
        cvr::camera::CamWriteRecordPush(eyeOrientation, genericHead);
        cvr::camera::CamWriteQuatPublish(
            eyeOrientation[0], eyeOrientation[1], eyeOrientation[2], eyeOrientation[3]);
    }
}

void __fastcall Detour_RttCameraRefresh(uintptr_t component) {
    const uintptr_t selectedVrcam = g_vrcam_comp.load(std::memory_order_acquire);
    if (component && component == selectedVrcam) {
        if (g_liveControls.xrAllowNonFppViews != 0) {
            PublishActiveMainToVrcam(component);
        } else {
            g_useTrueMain.store(false, std::memory_order_release);
        }
    }
    g_origRttCameraRefresh(component);
}

}  // namespace

CVR_DETOUR("[vrcam] generic active-MAIN handoff",
           RTT_CAMERA_REFRESH_RVA,
           Detour_RttCameraRefresh,
           g_origRttCameraRefresh)

}  // namespace cvr::detail
