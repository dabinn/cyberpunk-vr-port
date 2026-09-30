#include "Runtimes/OpenXRSwapchainFormat.hpp"

#include <cstdio>

namespace {
int failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

void TestRgbaSrgbPreferred() {
    const std::vector<int64_t> formats{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
    const auto selected = cvr::openxr::PickSwapchainFormat(formats, DXGI_FORMAT_R8G8B8A8_UNORM);
    Check(selected.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && !selected.decodeSrgbToLinear,
          "RGBA chooses its sRGB companion when both formats are available");
}

void TestBgraFamily() {
    const std::vector<int64_t> formats{DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    const auto selected = cvr::openxr::PickSwapchainFormat(formats, DXGI_FORMAT_B8G8R8A8_TYPELESS);
    Check(selected.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB && !selected.decodeSrgbToLinear,
          "BGRA typeless source chooses its sRGB companion");
}

void TestUnormFallbackRequiresDecode() {
    const std::vector<int64_t> formats{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT};
    const auto selected = cvr::openxr::PickSwapchainFormat(formats, DXGI_FORMAT_R8G8B8A8_UNORM);
    Check(selected.format == DXGI_FORMAT_R8G8B8A8_UNORM && selected.decodeSrgbToLinear,
          "matching UNORM fallback requests sRGB decoding");
}

void TestTypedSrgbSourceUsesUnormFallback() {
    const std::vector<int64_t> formats{DXGI_FORMAT_R8G8B8A8_UNORM};
    const auto selected = cvr::openxr::PickSwapchainFormat(formats, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    Check(selected.format == DXGI_FORMAT_R8G8B8A8_UNORM && selected.decodeSrgbToLinear,
          "typed sRGB source requests decoding for UNORM fallback");
}

void TestUnrelatedFormatRejected() {
    const std::vector<int64_t> formats{DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    const auto selected = cvr::openxr::PickSwapchainFormat(formats, DXGI_FORMAT_R8G8B8A8_UNORM);
    Check(selected.format == DXGI_FORMAT_UNKNOWN, "unrelated channel layout is rejected");
}
}

int main() {
    TestRgbaSrgbPreferred();
    TestBgraFamily();
    TestUnormFallbackRequiresDecode();
    TestTypedSrgbSourceUsesUnormFallback();
    TestUnrelatedFormatRejected();
    if (failures) {
        std::fprintf(stderr, "%d OpenXR swapchain format test(s) failed\n", failures);
        return 1;
    }
    std::puts("OpenXR swapchain format tests passed");
    return 0;
}
