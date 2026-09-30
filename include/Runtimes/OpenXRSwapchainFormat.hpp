#pragma once

#include <cstdint>
#include <d3d12.h>
#include <vector>

namespace cvr::openxr {

inline bool ContainsSwapchainFormat(const std::vector<int64_t>& formats, DXGI_FORMAT candidate) {
    for (const int64_t format : formats) {
        if (format == static_cast<int64_t>(candidate)) return true;
    }
    return false;
}

// OpenXR treats non-sRGB swapchain formats as linear. Preserve the source's sRGB
// encoding by selecting a bit-compatible sRGB format or explicitly decoding it.
struct SwapchainFormatSelection {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool decodeSrgbToLinear = false;
};

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

    if (ContainsSwapchainFormat(runtimeFormats, srgbFormat)) return {srgbFormat, false};
    if (ContainsSwapchainFormat(runtimeFormats, unormFormat)) return {unormFormat, true};
    return {};
}

} // namespace cvr::openxr
