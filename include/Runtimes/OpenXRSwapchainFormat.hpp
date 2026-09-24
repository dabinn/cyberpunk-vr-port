#pragma once

#include <cstdint>
#include <d3d12.h>
#include <vector>

namespace cvr::openxr {

inline bool ContainsSwapchainFormat(const std::vector<int64_t>& formats, DXGI_FORMAT candidate) {
    for (const int64_t format : formats) {
        if (format == static_cast<int64_t>(candidate)) {
            return true;
        }
    }
    return false;
}

// Both final eye images contain display-ready sRGB-encoded bytes: MAIN comes from the
// tonemapped game backbuffer, while VRCAM is encoded by its final sRGB RTV. OpenXR treats
// every non-sRGB swapchain format as linear, so submission must either use the
// bit-compatible sRGB member or decode the values before writing a linear format.
//
struct SwapchainFormatSelection {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool decodeSrgbToLinear = false;
};

// Prefer the sRGB member so the normal path remains a zero-cost bit copy. If a runtime only
// exposes the matching UNORM member, select it and require the caller to decode the source's
// sRGB values to linear values before submission. Formats from another bit layout are rejected
// because neither CopyResource nor the conversion pass performs channel or precision changes.
inline SwapchainFormatSelection PickSwapchainFormat(const std::vector<int64_t>& runtimeFormats,
                                                     DXGI_FORMAT sourceFormat) {
    DXGI_FORMAT srgbFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT unormFormat = DXGI_FORMAT_UNKNOWN;
    switch (sourceFormat) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            srgbFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            unormFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            srgbFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            unormFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            break;
        default:
            return {};
    }

    if (ContainsSwapchainFormat(runtimeFormats, srgbFormat)) {
        return {srgbFormat, false};
    }
    if (ContainsSwapchainFormat(runtimeFormats, unormFormat)) {
        return {unormFormat, true};
    }
    return {};
}

} // namespace cvr::openxr
