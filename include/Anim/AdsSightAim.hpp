#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "Core/LiveControls.hpp"
#include "Core/VrCoreShared.hpp"
#include "Utils/SharedSlots.hpp"

extern float* g_pSharedHands;
namespace overlay { extern bool g_drawBarrelCross; }

inline bool VrAdsSightAimActive() {
    return overlay::g_drawBarrelCross && g_liveControls.xrHideLaserDotAds != 0 && g_isAiming;
}

// Read a coherent weapon-owned sight origin. The firing paths use the muzzle when this is invalid.
inline bool VrReadSightOrigin(float* out) {
    if (!out || !g_pSharedHands) return false;
    for (int retry = 0; retry < 3; ++retry) {
        const uint32_t first = static_cast<uint32_t>(g_pSharedHands[vrshared::kSightOriginSeq]);
        if (first & 1u) continue;
        std::atomic_thread_fence(std::memory_order_acquire);
        const float x = g_pSharedHands[vrshared::kSightOriginX];
        const float y = g_pSharedHands[vrshared::kSightOriginX + 1];
        const float z = g_pSharedHands[vrshared::kSightOriginX + 2];
        const float valid = g_pSharedHands[vrshared::kSightOriginValid];
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t last = static_cast<uint32_t>(g_pSharedHands[vrshared::kSightOriginSeq]);
        const float lengthSq = x*x + y*y + z*z;
        if (first == last && !(last & 1u) && valid > 0.5f &&
            std::isfinite(lengthSq) && lengthSq > 1.0f) {
            out[0] = x;
            out[1] = y;
            out[2] = z;
            return true;
        }
    }
    return false;
}
