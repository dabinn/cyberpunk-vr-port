#include "Camera/ActiveMainCamera.hpp"

#include "Camera/CameraState.hpp"
#include "Core/VrCoreShared.hpp"
#include "Utils/MemorySafe.hpp"

#include <RED4ext/RED4ext.hpp>
#include <RED4ext/Scripting/Natives/entEntity.hpp>

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

struct MainCameraStorage {
    std::atomic<uint32_t> lock{0};
    std::atomic<int32_t> position[3]{};
    std::atomic<uint32_t> orientation[4]{};
    std::atomic<uint64_t> sequence{0};
    std::atomic<uint64_t> tickMs{0};
};

MainCameraStorage g_mainCamera;

uint32_t FloatBits(float value) {
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float BitsFloat(uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool ReadSnapshot(int32_t position[3], float orientation[4], uint64_t* sequence) {
    if (!position || !orientation) return false;

    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t before = g_mainCamera.lock.load(std::memory_order_acquire);
        if (before == 0 || (before & 1u)) continue;

        const uint64_t seq = g_mainCamera.sequence.load(std::memory_order_relaxed);
        const uint64_t tick = g_mainCamera.tickMs.load(std::memory_order_relaxed);
        for (int i = 0; i < 3; ++i) {
            position[i] = g_mainCamera.position[i].load(std::memory_order_relaxed);
        }
        for (int i = 0; i < 4; ++i) {
            orientation[i] = BitsFloat(
                g_mainCamera.orientation[i].load(std::memory_order_relaxed));
        }

        if (g_mainCamera.lock.load(std::memory_order_acquire) == before) {
            if (seq == 0 || GetTickCount64() - tick > 500 ||
                !IsPlausibleUnitQuaternion(orientation)) {
                return false;
            }
            if (sequence) *sequence = seq;
            return true;
        }
    }
    return false;
}

int32_t ReadWorldEntityStatusSafe(RED4ext::ent::Entity* entity) {
    if (!entity) return -1;
    __try {
        return static_cast<int32_t>(entity->status);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

bool IsWorldEntityTransformReadySafe(RED4ext::ent::Entity* entity) {
    if (!entity) return false;
    __try {
        return entity->status == RED4ext::EntityStatus::Attached &&
               entity->transformComponent != nullptr && entity->runtimeScene != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

namespace cvr::camera {

bool PublishActiveMainCamera(const float* renderCamera) {
    if (!renderCamera || reinterpret_cast<uintptr_t>(renderCamera) < 0x10000) return false;

    int32_t position[3] = {};
    float orientation[4] = {};
    const uintptr_t address = reinterpret_cast<uintptr_t>(renderCamera);
    for (int i = 0; i < 3; ++i) {
        uint32_t bits = 0;
        if (!ReadU32Safe(address + static_cast<uintptr_t>(i) * 4, &bits)) return false;
        position[i] = static_cast<int32_t>(bits);
    }
    if (!ReadFloatArraySafe(renderCamera + 4, orientation, 4) ||
        !IsPlausibleUnitQuaternion(orientation)) {
        return false;
    }

    const uint32_t odd = g_mainCamera.lock.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    if ((odd & 1u) == 0u) {
        g_mainCamera.lock.fetch_add(1u, std::memory_order_acq_rel);
    }
    for (int i = 0; i < 3; ++i) {
        g_mainCamera.position[i].store(position[i], std::memory_order_relaxed);
    }
    for (int i = 0; i < 4; ++i) {
        g_mainCamera.orientation[i].store(FloatBits(orientation[i]), std::memory_order_relaxed);
    }
    const uint64_t sequence = g_mainCamera.sequence.fetch_add(1u, std::memory_order_relaxed) + 1u;
    g_mainCamera.tickMs.store(GetTickCount64(), std::memory_order_relaxed);
    g_mainCamera.lock.fetch_add(1u, std::memory_order_release);

    static std::atomic<uint64_t> lastLogMs{0};
    const uint64_t now = GetTickCount64();
    uint64_t last = lastLogMs.load(std::memory_order_relaxed);
    if ((!last || now - last >= 1000) &&
        lastLogMs.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        Log("[worldvrcam][main-source] seq=%llu pos=(%.3f,%.3f,%.3f) "
            "q=(%.4f,%.4f,%.4f,%.4f)\n",
            static_cast<unsigned long long>(sequence),
            position[0] / 131072.0f, position[1] / 131072.0f,
            position[2] / 131072.0f,
            orientation[0], orientation[1], orientation[2], orientation[3]);
    }
    return true;
}

bool ReadWorldVrcamPose(int32_t position[3], float orientation[4], uint64_t* sequence) {
    if (!position || !orientation) return false;

    int32_t mainPosition[3] = {};
    if (!ReadSnapshot(mainPosition, orientation, sequence)) return false;

    float right[3] = {};
    ComputeRightVectorFromQuaternion(orientation, right);
    if (!IsPlausibleUnitVector3(right)) return false;

    const float ipd = 2.0f * GetDesiredHalfIpd();
    const float sign = CyberpunkVR_MainIsRightEye ? -1.0f : 1.0f;
    for (int i = 0; i < 3; ++i) {
        const int32_t eyeOffset = static_cast<int32_t>(
            std::lround(right[i] * ipd * sign * 131072.0f));
        position[i] = mainPosition[i] + eyeOffset;
    }
    return true;
}

} // namespace cvr::camera

void VRMainCameraWorldPos(RED4ext::IScriptable* aContext, RED4ext::CStackFrame* aFrame,
                          RED4ext::Vector4* aOut, int64_t a4) {
    RED4EXT_UNUSED_PARAMETER(aContext);
    RED4EXT_UNUSED_PARAMETER(a4);
    aFrame->code++;
    if (!aOut) return;

    int32_t position[3] = {};
    float orientation[4] = {};
    if (!ReadSnapshot(position, orientation, nullptr)) {
        aOut->X = 0.0f;
        aOut->Y = 0.0f;
        aOut->Z = 0.0f;
        aOut->W = 0.0f;
        return;
    }
    aOut->X = position[0] / 131072.0f;
    aOut->Y = position[1] / 131072.0f;
    aOut->Z = position[2] / 131072.0f;
    aOut->W = 1.0f;
}

void VRMainCameraWorldRot(RED4ext::IScriptable* aContext, RED4ext::CStackFrame* aFrame,
                          RED4ext::Quaternion* aOut, int64_t a4) {
    RED4EXT_UNUSED_PARAMETER(aContext);
    RED4EXT_UNUSED_PARAMETER(a4);
    aFrame->code++;
    if (!aOut) return;

    int32_t position[3] = {};
    float orientation[4] = {};
    if (!ReadSnapshot(position, orientation, nullptr)) {
        aOut->i = 0.0f;
        aOut->j = 0.0f;
        aOut->k = 0.0f;
        aOut->r = 1.0f;
        return;
    }
    aOut->i = orientation[0];
    aOut->j = orientation[1];
    aOut->k = orientation[2];
    aOut->r = orientation[3];
}

void VRVrcamWorldPos(RED4ext::IScriptable* aContext, RED4ext::CStackFrame* aFrame,
                     RED4ext::Vector4* aOut, int64_t a4) {
    RED4EXT_UNUSED_PARAMETER(aContext);
    RED4EXT_UNUSED_PARAMETER(a4);
    aFrame->code++;
    if (!aOut) return;

    int32_t position[3] = {};
    float orientation[4] = {};
    uint64_t sequence = 0;
    if (!ReadSnapshot(position, orientation, &sequence)) {
        aOut->X = 0.0f;
        aOut->Y = 0.0f;
        aOut->Z = 0.0f;
        aOut->W = 0.0f;
        return;
    }

    float right[3] = {};
    ComputeRightVectorFromQuaternion(orientation, right);
    const float ipd = 2.0f * GetDesiredHalfIpd();
    const float vrcamSign = CyberpunkVR_MainIsRightEye ? -1.0f : 1.0f;
    aOut->X = position[0] / 131072.0f + right[0] * ipd * vrcamSign;
    aOut->Y = position[1] / 131072.0f + right[1] * ipd * vrcamSign;
    aOut->Z = position[2] / 131072.0f + right[2] * ipd * vrcamSign;
    aOut->W = 1.0f;

    static std::atomic<uint64_t> lastLogMs{0};
    const uint64_t now = GetTickCount64();
    uint64_t last = lastLogMs.load(std::memory_order_relaxed);
    if ((!last || now - last >= 1000) &&
        lastLogMs.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        Log("[worldvrcam][target] seq=%llu main=(%.3f,%.3f,%.3f) "
            "target=(%.3f,%.3f,%.3f) ipd=%.4f eye=%s\n",
            static_cast<unsigned long long>(sequence),
            position[0] / 131072.0f, position[1] / 131072.0f,
            position[2] / 131072.0f,
            aOut->X, aOut->Y, aOut->Z, ipd,
            CyberpunkVR_MainIsRightEye ? "LEFT" : "RIGHT");
    }
}

void VRMainCameraSeq(RED4ext::IScriptable* aContext, RED4ext::CStackFrame* aFrame,
                     int32_t* aOut, int64_t a4) {
    RED4EXT_UNUSED_PARAMETER(aContext);
    RED4EXT_UNUSED_PARAMETER(a4);
    aFrame->code++;
    if (!aOut) return;

    int32_t position[3] = {};
    float orientation[4] = {};
    uint64_t sequence = 0;
    if (!ReadSnapshot(position, orientation, &sequence)) {
        *aOut = 0;
        return;
    }
    *aOut = static_cast<int32_t>(sequence & 0x7FFFFFFFu);
}

void VRWorldEntityStatus(RED4ext::IScriptable* aContext, RED4ext::CStackFrame* aFrame,
                         int32_t* aOut, int64_t a4) {
    RED4EXT_UNUSED_PARAMETER(aContext);
    RED4EXT_UNUSED_PARAMETER(a4);
    RED4ext::Handle<RED4ext::IScriptable> handle;
    RED4ext::GetParameter(aFrame, &handle);
    aFrame->code++;
    if (!aOut) return;
    *aOut = ReadWorldEntityStatusSafe(
        reinterpret_cast<RED4ext::ent::Entity*>(handle.instance));
}

void VRWorldEntityReady(RED4ext::IScriptable* aContext, RED4ext::CStackFrame* aFrame,
                        bool* aOut, int64_t a4) {
    RED4EXT_UNUSED_PARAMETER(aContext);
    RED4EXT_UNUSED_PARAMETER(a4);
    RED4ext::Handle<RED4ext::IScriptable> handle;
    RED4ext::GetParameter(aFrame, &handle);
    aFrame->code++;
    if (!aOut) return;
    *aOut = IsWorldEntityTransformReadySafe(
        reinterpret_cast<RED4ext::ent::Entity*>(handle.instance));
}
