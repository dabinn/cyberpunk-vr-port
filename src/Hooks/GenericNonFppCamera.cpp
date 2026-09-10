// Generic HMD composition for CameraDirector-owned non-FPP cameras.
//
// Vehicle TPP and R3 rear view serialize through sub_1407FFBD0. Compose the HMD only into that
// temporary CameraSetup, leaving the game's orbit camera state untouched. The same hook also copies
// the last stable FPP FOV into detached CameraSetup output; HMD testing on the 0.1.5 prototype showed
// this removes the external-camera zoom-in and rotation shear while preserving the game's distance
// presets. Room-scale translation is applied to the same temporary CameraSetup so position and
// orientation share one camera authority and one OpenXR sample.

#include "Camera/CameraLink.hpp"
#include "Camera/CameraState.hpp"
#include "Core/LiveControls.hpp"
#include "Hooks/Hook.hpp"
#include "Runtimes/OpenXRManager.hpp"
#include "Stereo/StereoInternal.hpp"
#include "Utils/MemorySafe.hpp"

#include <MinHook.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {

constexpr uintptr_t kCameraDirectorBlendRva = 0x12752C;
constexpr uintptr_t kFppSerializerRva = 0x127F58;
constexpr uintptr_t kGenericNonFppSerializerRva = 0x7FFBD0;

using CameraDirectorBlendFn = void (*)(void* director);
using CameraSetupSerializerFn = void (*)(void* interfaceObject, void* setup);

CameraDirectorBlendFn g_origCameraDirectorBlend = nullptr;
CameraSetupSerializerFn g_origGenericNonFppSerializer = nullptr;
std::atomic<float> g_lastStableFppFov{0.0f};
std::atomic<uint32_t> g_genericFrameSequence{0};
std::atomic<uint32_t> g_currentBlendSequence{0};
std::atomic<uintptr_t> g_lastCameraDirector{0};

struct CameraBlendEntry {
    uintptr_t camera = 0;
    uintptr_t serializer = 0;
    float weight = 0.0f;
};

uint32_t CaptureCameraBlendEntries(void* director, CameraBlendEntry outEntries[8], uint32_t* outActiveCount) {
    if (outActiveCount) *outActiveCount = 0;
    if (!director || !outEntries) return 0;

    const uintptr_t directorAddr = reinterpret_cast<uintptr_t>(director);
    uintptr_t table = 0;
    uint32_t countWord = 0;
    if (!ReadPtrSafe(directorAddr + 0x48u, &table) || !table ||
        !ReadU32Safe(directorAddr + 0x54u, &countWord)) {
        return 0;
    }

    const uint32_t tableCount = countWord & 0xFFu;
    uint32_t activeCount = 0;
    uint32_t captured = 0;
    for (uint32_t i = 0; i < tableCount; ++i) {
        const uintptr_t slot = table + static_cast<uintptr_t>(i) * 0x20u;
        float weight = 0.0f;
        if (!ReadFloatSafe(slot + 0x10u, &weight) || weight == 0.0f) continue;
        ++activeCount;
        if (captured >= 8) continue;

        CameraBlendEntry entry{};
        entry.weight = weight;
        if (ReadPtrSafe(slot, &entry.camera) && entry.camera) {
            const uintptr_t interfaceObject = entry.camera + 0x120u;
            uintptr_t vtable = 0;
            if (ReadPtrSafe(interfaceObject, &vtable) && vtable) {
                ReadPtrSafe(vtable + 0x20u, &entry.serializer);
            }
        }
        outEntries[captured++] = entry;
    }
    if (outActiveCount) *outActiveCount = activeCount;
    return captured;
}

bool HasGenericSerializer(const CameraBlendEntry entries[8], uint32_t capturedCount) {
    if (!cvr::detail::g_exe_base) return false;
    const uintptr_t target = reinterpret_cast<uintptr_t>(cvr::detail::g_exe_base) +
                             kGenericNonFppSerializerRva;
    for (uint32_t i = 0; i < capturedCount; ++i) {
        if (entries[i].serializer == target) return true;
    }
    return false;
}

bool FindPreferredHead(const CameraBlendEntry entries[8], uint32_t capturedCount,
                       OpenXRHeadPose* outHead) {
    if (!outHead) return false;
    const uintptr_t mainObject = g_camObjMain.load(std::memory_order_acquire);
    if (mainObject >= 0x10000) {
        for (uint32_t i = 0; i < capturedCount; ++i) {
            if (entries[i].camera != mainObject) continue;
            float q[4]{};
            if (ReadFloatArraySafe(reinterpret_cast<const float*>(mainObject + 0xF0u), q, 4) &&
                IsPlausibleUnitQuaternion(q) &&
                cvr::camera::CamWriteRecordFindExact(q, outHead) && outHead->valid) {
                return true;
            }
        }
    }
    return OpenXRManager::Get().AcquireFrameHeadSample(outHead) && outHead->valid;
}

bool FindCurrentBlendHead(const CameraBlendEntry entries[8], uint32_t capturedCount,
                          OpenXRHeadPose* outHead) {
    if (!outHead) return false;
    const uintptr_t mainObject = g_camObjMain.load(std::memory_order_acquire);
    bool hasMain = false;
    for (uint32_t i = 0; i < capturedCount; ++i) {
        if (entries[i].camera != mainObject) continue;
        hasMain = true;
        float q[4]{};
        if (ReadFloatArraySafe(reinterpret_cast<const float*>(mainObject + 0xF0u), q, 4) &&
            IsPlausibleUnitQuaternion(q) &&
            cvr::camera::CamWriteRecordFindExact(q, outHead) && outHead->valid) {
            return true;
        }
        break;
    }
    if (hasMain) return false;
    return OpenXRManager::Get().AcquireFrameHeadSample(outHead) && outHead->valid;
}

bool ComputeHeadWorldDelta(const float base[4], const OpenXRHeadPose& head,
                           float outWorldDelta[3]) {
    if (!base || !outWorldDelta || !IsPlausibleUnitQuaternion(base) || !head.valid) return false;

    // Keep room-scale translation in a level tracking frame. Gameplay camera pitch/roll should not
    // rotate a physical lean into vertical motion; only the detached camera's clean yaw turns the
    // OpenXR local right/forward axes into world space.
    const float flatYaw = atan2f(2.0f * (base[3] * base[2] + base[0] * base[1]),
                                 1.0f - 2.0f * (base[1] * base[1] + base[2] * base[2]));
    const float cy = cosf(flatYaw);
    const float sy = sinf(flatYaw);
    const float scale = GetWorldScale();
    const float localRight = head.posX * scale;
    const float localForward = -head.posZ * scale;
    const float localUp = head.posY * scale;

    outWorldDelta[0] = cy * localRight - sy * localForward;
    outWorldDelta[1] = sy * localRight + cy * localForward;
    outWorldDelta[2] = localUp;
    return std::isfinite(outWorldDelta[0]) && std::isfinite(outWorldDelta[1]) &&
           std::isfinite(outWorldDelta[2]);
}

bool ComposeHeadOrientation(const float base[4], const OpenXRHeadPose& head, float out[4]) {
    if (!base || !out || !IsPlausibleUnitQuaternion(base) || !head.valid) return false;
    MulQuat(base[0], base[1], base[2], base[3],
            head.oriX, -head.oriZ, head.oriY, head.oriW,
            out[0], out[1], out[2], out[3]);
    NormalizeQuat(out[0], out[1], out[2], out[3]);
    return IsPlausibleUnitQuaternion(out);
}

bool ReadFixedPosition(uintptr_t address, float out[3]) {
    if (!address || !out) return false;
    for (int i = 0; i < 3; ++i) {
        uint32_t bits = 0;
        if (!ReadU32Safe(address + static_cast<uintptr_t>(i) * 4u, &bits)) return false;
        out[i] = static_cast<float>(static_cast<int32_t>(bits)) / 131072.0f;
    }
    return true;
}

bool ReadEntryPose(const CameraBlendEntry& entry, const OpenXRHeadPose& head,
                   uintptr_t mainObject, float outPos[3], float outQuat[4]) {
    if (!entry.camera || !entry.serializer || !outPos || !outQuat) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(cvr::detail::g_exe_base);
    if (!base) return false;

    if (entry.serializer == base + kFppSerializerRva) {
        if (entry.camera != mainObject) return false;
        return ReadFixedPosition(entry.camera + 0xE0u, outPos) &&
               ReadFloatArraySafe(reinterpret_cast<const float*>(entry.camera + 0xF0u), outQuat, 4) &&
               IsPlausibleUnitQuaternion(outQuat);
    }

    if (entry.serializer != base + kGenericNonFppSerializerRva) return false;

    float cleanBase[4]{};
    float basePos[3]{};
    float worldDelta[3]{};
    if (!ReadFixedPosition(entry.camera + 0x3A0u, basePos) ||
        !ReadFloatArraySafe(reinterpret_cast<const float*>(entry.camera + 0x3B0u), cleanBase, 4) ||
        !IsPlausibleUnitQuaternion(cleanBase) ||
        !ComputeHeadWorldDelta(cleanBase, head, worldDelta) ||
        !ComposeHeadOrientation(cleanBase, head, outQuat)) {
        return false;
    }
    for (int i = 0; i < 3; ++i) outPos[i] = basePos[i] + worldDelta[i];
    return true;
}

cvr::camera::GenericNonFppCurrentBlendState BuildCurrentBlend(
    cvr::camera::GenericNonFppCameraFrame* out) {
    if (!out || !cvr::detail::g_exe_base) {
        return cvr::camera::GenericNonFppCurrentBlendState::Unavailable;
    }
    *out = {};

    const uintptr_t director = g_lastCameraDirector.load(std::memory_order_acquire);
    if (director < 0x10000u) {
        return cvr::camera::GenericNonFppCurrentBlendState::Unavailable;
    }

    CameraBlendEntry entries[8]{};
    uint32_t activeCount = 0;
    const uint32_t capturedCount =
        CaptureCameraBlendEntries(reinterpret_cast<void*>(director), entries, &activeCount);
    if (activeCount == 0 || capturedCount != activeCount || capturedCount > 8) {
        return cvr::camera::GenericNonFppCurrentBlendState::Unavailable;
    }

    const uintptr_t exeBase = reinterpret_cast<uintptr_t>(cvr::detail::g_exe_base);
    const uintptr_t genericSerializer = exeBase + kGenericNonFppSerializerRva;
    const uintptr_t fppSerializer = exeBase + kFppSerializerRva;
    const uintptr_t mainObject = g_camObjMain.load(std::memory_order_acquire);
    bool hasGeneric = false;
    for (uint32_t i = 0; i < capturedCount; ++i) {
        if (!(entries[i].weight > 0.0f) || !std::isfinite(entries[i].weight)) {
            return cvr::camera::GenericNonFppCurrentBlendState::Unavailable;
        }
        hasGeneric = hasGeneric || entries[i].serializer == genericSerializer;
    }
    if (!hasGeneric) return cvr::camera::GenericNonFppCurrentBlendState::NoGeneric;
    for (uint32_t i = 0; i < capturedCount; ++i) {
        if (entries[i].serializer == genericSerializer) continue;
        if (entries[i].serializer != fppSerializer || entries[i].camera != mainObject) {
            return cvr::camera::GenericNonFppCurrentBlendState::Unsupported;
        }
    }

    OpenXRHeadPose head{};
    if (!FindCurrentBlendHead(entries, capturedCount, &head) || !head.valid) {
        return cvr::camera::GenericNonFppCurrentBlendState::Unavailable;
    }

    float blendedPos[3]{};
    float blendedQuat[4]{};
    float hemisphereBase[4]{};
    bool haveQuat = false;
    for (uint32_t i = 0; i < capturedCount; ++i) {
        float pos[3]{};
        float quat[4]{};
        if (!ReadEntryPose(entries[i], head, mainObject, pos, quat)) {
            return cvr::camera::GenericNonFppCurrentBlendState::Unavailable;
        }

        if (capturedCount == 1u) {
            for (int axis = 0; axis < 3; ++axis) blendedPos[axis] = pos[axis];
            for (int axis = 0; axis < 4; ++axis) blendedQuat[axis] = quat[axis];
            haveQuat = true;
            break;
        }

        const float weight = entries[i].weight;
        for (int axis = 0; axis < 3; ++axis) blendedPos[axis] += pos[axis] * weight;

        if (!haveQuat) {
            for (int axis = 0; axis < 4; ++axis) hemisphereBase[axis] = quat[axis];
            haveQuat = true;
        } else {
            float dot = 0.0f;
            for (int axis = 0; axis < 4; ++axis) dot += hemisphereBase[axis] * quat[axis];
            if (dot < 0.0f) {
                for (float& value : quat) value = -value;
            }
        }
        for (int axis = 0; axis < 4; ++axis) blendedQuat[axis] += quat[axis] * weight;
    }

    if (!haveQuat) return cvr::camera::GenericNonFppCurrentBlendState::Unavailable;
    NormalizeQuat(blendedQuat[0], blendedQuat[1], blendedQuat[2], blendedQuat[3]);
    if (!IsPlausibleUnitQuaternion(blendedQuat)) {
        return cvr::camera::GenericNonFppCurrentBlendState::Unavailable;
    }

    float cleanBase[4]{};
    MulQuat(blendedQuat[0], blendedQuat[1], blendedQuat[2], blendedQuat[3],
            -head.oriX, head.oriZ, -head.oriY, head.oriW,
            cleanBase[0], cleanBase[1], cleanBase[2], cleanBase[3]);
    NormalizeQuat(cleanBase[0], cleanBase[1], cleanBase[2], cleanBase[3]);
    if (!IsPlausibleUnitQuaternion(cleanBase)) {
        return cvr::camera::GenericNonFppCurrentBlendState::Unavailable;
    }

    for (int axis = 0; axis < 3; ++axis) out->worldPos[axis] = blendedPos[axis];
    for (int axis = 0; axis < 4; ++axis) out->worldQuat[axis] = cleanBase[axis];
    out->hmdPose = head;
    out->timestampUs = XrDiagNowUs();
    out->sequence = g_currentBlendSequence.fetch_add(1u, std::memory_order_relaxed) + 1u;
    out->active = 1u;
    out->hmdComposed = 1u;

    return cvr::camera::GenericNonFppCurrentBlendState::Ready;
}

bool AddHeadTranslationToSetup(uintptr_t setupAddr, const float base[4],
                               const OpenXRHeadPose& head) {
    if (!setupAddr || !base) return false;

    int32_t* const position = reinterpret_cast<int32_t*>(setupAddr);
    float worldDelta[3]{};
    if (!ComputeHeadWorldDelta(base, head, worldDelta)) return false;

    for (int i = 0; i < 3; ++i) {
        const int64_t delta = static_cast<int64_t>(std::llround(worldDelta[i] * 131072.0f));
        const int64_t shifted = static_cast<int64_t>(position[i]) + delta;
        if (shifted < std::numeric_limits<int32_t>::min() ||
            shifted > std::numeric_limits<int32_t>::max()) {
            return false;
        }
        position[i] = static_cast<int32_t>(shifted);
    }
    return true;
}

void Hooked_GenericNonFppSerializer(void* interfaceObject, void* setup) {
    if (g_origGenericNonFppSerializer) g_origGenericNonFppSerializer(interfaceObject, setup);
    if (g_liveControls.xrAllowNonFppViews == 0 || !interfaceObject || !setup) return;

    const uintptr_t interfaceAddr = reinterpret_cast<uintptr_t>(interfaceObject);
    if (interfaceAddr < 0x120u) return;
    const uintptr_t cameraObject = interfaceAddr - 0x120u;
    if (!cvr::camera::CameraDirectorBlendScopeContains(cameraObject)) return;

    OpenXRHeadPose head{};
    if (!cvr::camera::CameraDirectorBlendScopeReadHead(cameraObject, &head) || !head.valid) {
        if (!OpenXRManager::Get().AcquireFrameHeadSample(&head) || !head.valid) return;
    }

    const uintptr_t setupAddr = reinterpret_cast<uintptr_t>(setup);
    float externalFov = 0.0f;
    const float fppFov = g_lastStableFppFov.load(std::memory_order_acquire);
    const bool externalFovOk = ReadFloatSafe(setupAddr + 0x20u, &externalFov) &&
        std::isfinite(externalFov) && externalFov > 1.0f && externalFov < 179.0f;
    const bool fppFovOk = std::isfinite(fppFov) && fppFov > 1.0f && fppFov < 179.0f;
    if (externalFovOk && fppFovOk) WriteFloatSafe(setupAddr + 0x20u, fppFov);

    float base[4]{};
    if (!ReadFloatArraySafe(reinterpret_cast<const float*>(setupAddr + 0x10u), base, 4) ||
        !IsPlausibleUnitQuaternion(base)) {
        return;
    }

    if (!AddHeadTranslationToSetup(setupAddr, base, head)) return;

    float composed[4]{};
    if (!ComposeHeadOrientation(base, head, composed)) return;

    if (!WriteFloatSafe(setupAddr + 0x10u, composed[0]) ||
        !WriteFloatSafe(setupAddr + 0x14u, composed[1]) ||
        !WriteFloatSafe(setupAddr + 0x18u, composed[2]) ||
        !WriteFloatSafe(setupAddr + 0x1Cu, composed[3])) {
        return;
    }

    if (!cvr::camera::CameraDirectorBlendScopeMarkComposed(cameraObject, head)) return;
}

void Hooked_CameraDirectorBlend(void* director) {
    if (director) {
        g_lastCameraDirector.store(reinterpret_cast<uintptr_t>(director), std::memory_order_release);
    }
    CameraBlendEntry entries[8]{};
    uint32_t activeCount = 0;
    const uint32_t capturedCount = g_liveControls.xrAllowNonFppViews != 0
        ? CaptureCameraBlendEntries(director, entries, &activeCount)
        : 0u;
    const bool hasGeneric = g_liveControls.xrAllowNonFppViews != 0 &&
                            HasGenericSerializer(entries, capturedCount);

    if (hasGeneric) {
        uintptr_t cameraObjects[8]{};
        for (uint32_t i = 0; i < capturedCount; ++i) cameraObjects[i] = entries[i].camera;
        OpenXRHeadPose preferredHead{};
        const bool havePreferredHead = FindPreferredHead(entries, capturedCount, &preferredHead);
        cvr::camera::CameraDirectorBlendScopeBegin(
            cameraObjects, capturedCount, activeCount,
            havePreferredHead ? &preferredHead : nullptr);
    }

    if (g_origCameraDirectorBlend) g_origCameraDirectorBlend(director);

    const uintptr_t mainObject = g_camObjMain.load(std::memory_order_acquire);
    if (director && activeCount == 1u && capturedCount == 1u &&
        entries[0].camera != 0 && entries[0].camera == mainObject) {
        float stableFppFov = 0.0f;
        const uintptr_t directorAddr = reinterpret_cast<uintptr_t>(director);
        if (ReadFloatSafe(directorAddr + 0x4E0u, &stableFppFov) &&
            std::isfinite(stableFppFov) && stableFppFov > 1.0f && stableFppFov < 179.0f) {
            g_lastStableFppFov.store(stableFppFov, std::memory_order_release);
        }
    }

    if (director) {
        cvr::camera::GenericNonFppCameraFrame genericFrame{};
        genericFrame.timestampUs = XrDiagNowUs();
        genericFrame.sequence = g_genericFrameSequence.fetch_add(1u, std::memory_order_relaxed) + 1u;

        OpenXRHeadPose blendHead{};
        float blendQuat[4]{};
        const uintptr_t directorAddr = reinterpret_cast<uintptr_t>(director);
        const bool blendQuatOk =
            ReadFloatArraySafe(reinterpret_cast<const float*>(directorAddr + 0x4D0u), blendQuat, 4) &&
            IsPlausibleUnitQuaternion(blendQuat);
        const bool allComposed = hasGeneric &&
            cvr::camera::CameraDirectorBlendScopeAllComposed(&blendHead) && blendHead.valid;

        if (hasGeneric && blendQuatOk) {
            uint32_t posBits[3]{};
            bool positionOk = true;
            for (int i = 0; i < 3; ++i) {
                positionOk = positionOk &&
                    ReadU32Safe(directorAddr + 0x4C0u + static_cast<uintptr_t>(i) * 4u, &posBits[i]);
                genericFrame.worldPos[i] =
                    static_cast<float>(static_cast<int32_t>(posBits[i])) / 131072.0f;
            }

            if (positionOk) {
                for (int i = 0; i < 4; ++i) genericFrame.worldQuat[i] = blendQuat[i];
                genericFrame.active = 1u;

                if (allComposed) {
                    float cleanBase[4]{};
                    MulQuat(blendQuat[0], blendQuat[1], blendQuat[2], blendQuat[3],
                            -blendHead.oriX, blendHead.oriZ, -blendHead.oriY, blendHead.oriW,
                            cleanBase[0], cleanBase[1], cleanBase[2], cleanBase[3]);
                    NormalizeQuat(cleanBase[0], cleanBase[1], cleanBase[2], cleanBase[3]);
                    if (IsPlausibleUnitQuaternion(cleanBase)) {
                        for (int i = 0; i < 4; ++i) genericFrame.worldQuat[i] = cleanBase[i];
                        genericFrame.hmdPose = blendHead;
                        genericFrame.hmdComposed = 1u;
                    }
                }
            }
        }

        cvr::camera::GenericNonFppCameraFramePublish(genericFrame);

        if (hasGeneric && allComposed && blendQuatOk) {
            cvr::camera::CamWriteRecordPush(blendQuat, blendHead);
            OpenXRManager::Get().PushRenderHeadPose(blendHead);
        }
    }

    if (director && hasGeneric) {
        cvr::camera::CameraDirectorBlendScopeEnd();
    }
}

bool InstallGenericNonFppCameraHooks() {
    if (!cvr::detail::g_exe_base) return false;
    uint8_t* const base = cvr::detail::g_exe_base;
    void* const blend = base + kCameraDirectorBlendRva;
    void* const serializer = base + kGenericNonFppSerializerRva;

    if (MH_CreateHook(blend, &Hooked_CameraDirectorBlend,
                      reinterpret_cast<void**>(&g_origCameraDirectorBlend)) != MH_OK) {
        return false;
    }
    if (MH_EnableHook(blend) != MH_OK) return false;

    if (MH_CreateHook(serializer, &Hooked_GenericNonFppSerializer,
                      reinterpret_cast<void**>(&g_origGenericNonFppSerializer)) != MH_OK) {
        return false;
    }
    return MH_EnableHook(serializer) == MH_OK;
}

}  // namespace

cvr::camera::GenericNonFppCurrentBlendState cvr::camera::GenericNonFppCurrentBlendRead(
    GenericNonFppCameraFrame* out) {
    return BuildCurrentBlend(out);
}

CVR_HOOK("GenericNonFppCamera", ::cvr::hooks::Stage::PostStereo, 80, InstallGenericNonFppCameraHooks);
