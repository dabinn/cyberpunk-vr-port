// Generic HMD composition for CameraDirector-owned non-FPP cameras.
//
// Vehicle TPP and R3 rear view serialize through sub_1407FFBD0. Compose the HMD only into that
// temporary CameraSetup, leaving the game's orbit camera state untouched. The same hook also copies
// the last stable FPP FOV into detached CameraSetup output; HMD testing on the 0.1.5 prototype showed
// this removes the external-camera zoom-in and rotation shear while preserving the game's distance
// presets. Position/room-scale ownership remains intentionally unresolved in this WIP.

#include "Camera/CameraLink.hpp"
#include "Camera/CameraState.hpp"
#include "Core/LiveControls.hpp"
#include "Hooks/Hook.hpp"
#include "Runtimes/OpenXRManager.hpp"
#include "Stereo/StereoInternal.hpp"
#include "Utils/MemorySafe.hpp"
#include "Utils/StereoLog.hpp"

#include <MinHook.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>

namespace {

constexpr uintptr_t kCameraDirectorBlendRva = 0x12752C;
constexpr uintptr_t kGenericNonFppSerializerRva = 0x7FFBD0;

using CameraDirectorBlendFn = void (*)(void* director);
using CameraSetupSerializerFn = void (*)(void* interfaceObject, void* setup);

CameraDirectorBlendFn g_origCameraDirectorBlend = nullptr;
CameraSetupSerializerFn g_origGenericNonFppSerializer = nullptr;
std::atomic<float> g_lastStableFppFov{0.0f};
std::atomic<uint64_t> g_composeCalls{0};
std::atomic<uint64_t> g_provenanceWrites{0};

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
    const bool fovCopied = externalFovOk && fppFovOk && WriteFloatSafe(setupAddr + 0x20u, fppFov);

    float base[4]{};
    if (!ReadFloatArraySafe(reinterpret_cast<const float*>(setupAddr + 0x10u), base, 4) ||
        !IsPlausibleUnitQuaternion(base)) {
        return;
    }

    float composed[4]{};
    MulQuat(base[0], base[1], base[2], base[3],
            head.oriX, -head.oriZ, head.oriY, head.oriW,
            composed[0], composed[1], composed[2], composed[3]);
    NormalizeQuat(composed[0], composed[1], composed[2], composed[3]);
    if (!IsPlausibleUnitQuaternion(composed)) return;

    if (!WriteFloatSafe(setupAddr + 0x10u, composed[0]) ||
        !WriteFloatSafe(setupAddr + 0x14u, composed[1]) ||
        !WriteFloatSafe(setupAddr + 0x18u, composed[2]) ||
        !WriteFloatSafe(setupAddr + 0x1Cu, composed[3])) {
        return;
    }

    if (!cvr::camera::CameraDirectorBlendScopeMarkComposed(cameraObject, head)) return;
    const uint64_t calls = g_composeCalls.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if ((calls % 240u) == 1u) {
        Log("[camera-nonfpp-compose] calls=%llu camera=%p epoch=%llu fovRaw=%.3f fppFov=%.3f copied=%d "
            "baseQ=(%.4f,%.4f,%.4f,%.4f) outQ=(%.4f,%.4f,%.4f,%.4f)\n",
            static_cast<unsigned long long>(calls),
            reinterpret_cast<void*>(cameraObject),
            static_cast<unsigned long long>(head.frameAimEpoch), externalFov, fppFov,
            fovCopied ? 1 : 0,
            base[0], base[1], base[2], base[3],
            composed[0], composed[1], composed[2], composed[3]);
    }
}

void Hooked_CameraDirectorBlend(void* director) {
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

    if (director && hasGeneric) {
        OpenXRHeadPose blendHead{};
        float blendQuat[4]{};
        const uintptr_t directorAddr = reinterpret_cast<uintptr_t>(director);
        if (cvr::camera::CameraDirectorBlendScopeAllComposed(&blendHead) &&
            ReadFloatArraySafe(reinterpret_cast<const float*>(directorAddr + 0x4D0u), blendQuat, 4) &&
            IsPlausibleUnitQuaternion(blendQuat)) {
            cvr::camera::CamWriteRecordPush(blendQuat, blendHead);
            OpenXRManager::Get().PushRenderHeadPose(blendHead);
            const uint64_t writes = g_provenanceWrites.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if ((writes % 240u) == 1u || activeCount > 1u) {
                Log("[camera-blend-provenance] writes=%llu active=%u epoch=%llu q=(%.4f,%.4f,%.4f,%.4f)\n",
                    static_cast<unsigned long long>(writes), activeCount,
                    static_cast<unsigned long long>(blendHead.frameAimEpoch),
                    blendQuat[0], blendQuat[1], blendQuat[2], blendQuat[3]);
            }
        }
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

CVR_HOOK("GenericNonFppCamera", ::cvr::hooks::Stage::PostStereo, 80, InstallGenericNonFppCameraHooks);
