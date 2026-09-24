#include "Runtimes/OpenXRSwapchainFormat.hpp"

#include <cstdio>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

void TestRgbaSrgbPreferredOverMatchingUnorm() {
    const std::vector<int64_t> formats = {
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
    };
    const auto selected = cvr::openxr::PickSwapchainFormat(formats, DXGI_FORMAT_R8G8B8A8_UNORM);
    Check(selected.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && !selected.decodeSrgbToLinear,
          "RGBA sRGB source selects the sRGB companion even when UNORM is available");
}

void TestBgraSrgbFamily() {
    const std::vector<int64_t> formats = {
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
    };
    const auto selected = cvr::openxr::PickSwapchainFormat(formats, DXGI_FORMAT_B8G8R8A8_TYPELESS);
    Check(selected.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB && !selected.decodeSrgbToLinear,
          "BGRA typeless source selects its bit-compatible sRGB companion");
}

void TestMissingSrgbUsesConvertedUnormFallback() {
    const std::vector<int64_t> formats = {
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT
    };
    const auto selected = cvr::openxr::PickSwapchainFormat(formats, DXGI_FORMAT_R8G8B8A8_UNORM);
    Check(selected.format == DXGI_FORMAT_R8G8B8A8_UNORM && selected.decodeSrgbToLinear,
          "missing sRGB support selects matching UNORM with explicit conversion");
}

void TestTypedSrgbSourceCanUseConvertedUnormFallback() {
    const std::vector<int64_t> formats = {
        DXGI_FORMAT_R8G8B8A8_UNORM
    };
    const auto selected = cvr::openxr::PickSwapchainFormat(
        formats, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    Check(selected.format == DXGI_FORMAT_R8G8B8A8_UNORM && selected.decodeSrgbToLinear,
          "typed sRGB source uses the explicit linear UNORM fallback");
}

void TestUnrelatedSrgbFamilyIsRejected() {
    const std::vector<int64_t> formats = {
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
    };
    const auto selected = cvr::openxr::PickSwapchainFormat(formats, DXGI_FORMAT_R8G8B8A8_UNORM);
    Check(selected.format == DXGI_FORMAT_UNKNOWN,
          "submit does not switch channel order to an unrelated sRGB family");
}

} // namespace

int main() {
    TestRgbaSrgbPreferredOverMatchingUnorm();
    TestBgraSrgbFamily();
    TestMissingSrgbUsesConvertedUnormFallback();
    TestTypedSrgbSourceCanUseConvertedUnormFallback();
    TestUnrelatedSrgbFamilyIsRejected();

    if (failures != 0) {
        std::fprintf(stderr, "%d OpenXR swapchain format test(s) failed\n", failures);
        return 1;
    }
    std::puts("OpenXR swapchain format tests passed");
    return 0;
}
