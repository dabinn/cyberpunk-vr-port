#include "Stereo/CetOverlayLayer.hpp"

#include "Overlay/ImGuiOverlay.hpp"
#include "Render/ColorBlit.hpp"

#include <windows.h>
#include <wrl.h>

#include <array>
#include <cwchar>
#include <memory>
#include <mutex>

extern void Log(const char* fmt, ...);

extern "C" __declspec(dllexport) int32_t  CyberpunkVR_CetStereoOverlay = 1;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCetCommandLists = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCetRtvs = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCetRedirectBarriers = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCetRedirectBinds = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCetMainComposites = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCetEyeComposites = 0;

namespace {
using Microsoft::WRL::ComPtr;

struct RtvBinding {
    SIZE_T handle = 0;
    ID3D12Resource* resource = nullptr;
};

std::mutex g_mutex;
std::array<ID3D12GraphicsCommandList*, 8> g_cetLists{};
uint32_t g_cetListCount = 0;
std::array<RtvBinding, 16> g_cetRtvs{};
uint32_t g_cetRtvCount = 0;

ComPtr<ID3D12Resource> g_layer;
ComPtr<ID3D12DescriptorHeap> g_layerRtvHeap;
D3D12_CPU_DESCRIPTOR_HANDLE g_layerRtv{};
D3D12_RESOURCE_DESC g_layerDesc{};
std::shared_ptr<ColorBlit> g_layerBlit;
bool g_layerHasFrame = false;

bool IsCetCaller(const void* returnAddress) {
    if (!returnAddress) return false;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(returnAddress), &module) || !module) {
        return false;
    }
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return false;
    const wchar_t* base = std::wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    return _wcsicmp(base, L"cyber_engine_tweaks.asi") == 0;
}

bool IsCetListLocked(ID3D12GraphicsCommandList* list) {
    for (uint32_t i = 0; i < g_cetListCount; ++i) {
        if (g_cetLists[i] == list) return true;
    }
    return false;
}

ID3D12Resource* FindCetRtvResourceLocked(SIZE_T handle) {
    for (uint32_t i = 0; i < g_cetRtvCount; ++i) {
        if (g_cetRtvs[i].handle == handle) return g_cetRtvs[i].resource;
    }
    return nullptr;
}

bool IsSupportedBackbufferFormat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM ||
           format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
}

bool EnsureLayerLocked(ID3D12Resource* backbuffer) {
    if (!backbuffer) return false;
    const D3D12_RESOURCE_DESC src = backbuffer->GetDesc();
    if (src.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        !IsSupportedBackbufferFormat(src.Format) || !src.Width || !src.Height) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            Log("[cet-layer] unsupported backbuffer format/dims fmt=%u %llux%u\n",
                static_cast<unsigned>(src.Format),
                static_cast<unsigned long long>(src.Width), src.Height);
        }
        return false;
    }

    if (g_layer && g_layerDesc.Width == src.Width && g_layerDesc.Height == src.Height &&
        g_layerDesc.SampleDesc.Count == src.SampleDesc.Count) {
        return true;
    }

    ComPtr<ID3D12Device> device;
    if (FAILED(backbuffer->GetDevice(IID_PPV_ARGS(&device))) || !device) return false;

    D3D12_RESOURCE_DESC desc = src;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

    ComPtr<ID3D12Resource> layer;
    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
        &clear, IID_PPV_ARGS(&layer));
    if (FAILED(hr) || !layer) {
        Log("[cet-layer] CreateCommittedResource failed hr=0x%08X\n",
            static_cast<unsigned>(hr));
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap));
    if (FAILED(hr) || !rtvHeap) {
        Log("[cet-layer] CreateDescriptorHeap failed hr=0x%08X\n",
            static_cast<unsigned>(hr));
        return false;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(layer.Get(), &rtvDesc, rtv);
    layer->SetName(L"CyberpunkVR_CETOverlayLayer");

    g_layer = std::move(layer);
    g_layerRtvHeap = std::move(rtvHeap);
    g_layerRtv = rtv;
    g_layerDesc = desc;
    g_layerBlit = std::make_shared<ColorBlit>();
    g_layerHasFrame = false;

    Log("[cet-layer] layer created %llux%u fmt=UNORM resource=%p\n",
        static_cast<unsigned long long>(desc.Width), desc.Height, g_layer.Get());
    return true;
}

}  // namespace

void CetOverlayNoteCommandListCreated(ID3D12GraphicsCommandList* list, const void* returnAddress) {
    if (!list || !IsCetCaller(returnAddress)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (IsCetListLocked(list)) return;
    if (g_cetListCount >= g_cetLists.size()) return;
    g_cetLists[g_cetListCount++] = list;
    CyberpunkVR_DebugCetCommandLists = g_cetListCount;
    Log("[cet-layer] tracked CET command list=%p count=%u\n", list, g_cetListCount);
}

void CetOverlayNoteRtvCreated(ID3D12Device*, ID3D12Resource* resource,
                              D3D12_CPU_DESCRIPTOR_HANDLE handle, const void* returnAddress) {
    if (!resource || !handle.ptr || !IsCetCaller(returnAddress)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (uint32_t i = 0; i < g_cetRtvCount; ++i) {
        if (g_cetRtvs[i].handle == handle.ptr) {
            g_cetRtvs[i].resource = resource;
            return;
        }
    }
    if (g_cetRtvCount >= g_cetRtvs.size()) return;
    g_cetRtvs[g_cetRtvCount++] = {handle.ptr, resource};
    CyberpunkVR_DebugCetRtvs = g_cetRtvCount;
    Log("[cet-layer] tracked CET RTV=%p resource=%p count=%u\n",
        reinterpret_cast<void*>(handle.ptr), resource, g_cetRtvCount);
}

bool CetOverlayOwnsCommandList(ID3D12GraphicsCommandList* list) {
    if (!list) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    return IsCetListLocked(list);
}

bool CetOverlayRewriteBarriers(ID3D12GraphicsCommandList* list, UINT count,
                               const D3D12_RESOURCE_BARRIER* barriers,
                               D3D12_RESOURCE_BARRIER* rewritten) {
    if (!CyberpunkVR_CetStereoOverlay || !list || !count || !barriers || !rewritten) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!IsCetListLocked(list)) return false;

    bool changed = false;
    for (UINT i = 0; i < count; ++i) {
        rewritten[i] = barriers[i];
        if (barriers[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
            !barriers[i].Transition.pResource) {
            continue;
        }
        ID3D12Resource* const resource = barriers[i].Transition.pResource;
        if (!overlay::OverlayIsSwapchainBackbuffer(resource) || !EnsureLayerLocked(resource)) continue;
        rewritten[i].Transition.pResource = g_layer.Get();
        changed = true;
        ++CyberpunkVR_DebugCetRedirectBarriers;
    }
    return changed;
}

bool CetOverlayRewriteRenderTargets(ID3D12GraphicsCommandList* list, UINT count,
                                    const D3D12_CPU_DESCRIPTOR_HANDLE* handles, BOOL contiguous,
                                    D3D12_CPU_DESCRIPTOR_HANDLE* rewritten,
                                    BOOL* rewrittenContiguous) {
    if (!CyberpunkVR_CetStereoOverlay || !list || !count || !handles || !rewritten ||
        !rewrittenContiguous || count > 8) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!IsCetListLocked(list)) return false;

    ComPtr<ID3D12Device> device;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))) || !device) return false;
    const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    bool changed = false;
    for (UINT i = 0; i < count; ++i) {
        rewritten[i].ptr = contiguous ? handles[0].ptr + static_cast<SIZE_T>(stride) * i
                                      : handles[i].ptr;
        ID3D12Resource* const resource = FindCetRtvResourceLocked(rewritten[i].ptr);
        if (!resource || !overlay::OverlayIsSwapchainBackbuffer(resource) || !EnsureLayerLocked(resource)) continue;
        rewritten[i] = g_layerRtv;
        changed = true;
    }

    if (!changed) return false;
    *rewrittenContiguous = FALSE;
    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    list->ClearRenderTargetView(g_layerRtv, clear, 0, nullptr);
    // The clear and CET's following ImGui draws are recorded on this same list. Our MAIN/VRCAM
    // composites are submitted later on the same DIRECT queue, so once this bind has been recorded
    // the layer has a defined producer before any later consumer executes.
    g_layerHasFrame = true;
    ++CyberpunkVR_DebugCetRedirectBinds;
    if (CyberpunkVR_DebugCetRedirectBinds <= 4) {
        Log("[cet-layer] redirected CET main RTV bind list=%p layer=%p\n", list, g_layer.Get());
    }
    return true;
}

bool CetOverlayRecordIntoTarget(ID3D12GraphicsCommandList* list, ID3D12Resource* target,
                                float shiftPx) {
    if (!CyberpunkVR_CetStereoOverlay || !list || !target) return false;

    ComPtr<ID3D12Resource> layer;
    std::shared_ptr<ColorBlit> blit;
    D3D12_RESOURCE_DESC layerDesc{};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_layer || !g_layerBlit || !g_layerHasFrame) return false;
        layer = g_layer;
        blit = g_layerBlit;
        layerDesc = g_layerDesc;
    }

    const D3D12_RESOURCE_DESC targetDesc = target->GetDesc();
    if (targetDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        !IsSupportedBackbufferFormat(targetDesc.Format) ||
        targetDesc.Width != layerDesc.Width || targetDesc.Height != layerDesc.Height) {
        return false;
    }

    ComPtr<ID3D12Device> device;
    if (FAILED(layer->GetDevice(IID_PPV_ARGS(&device))) || !device) return false;
    if (!blit->EnsureInitialized(device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                 static_cast<uint32_t>(targetDesc.Width), targetDesc.Height)) {
        return false;
    }
    return blit->RecordOverlay(list, layer.Get(), target, 0, true, shiftPx, 0.0f);
}

void CetOverlayInvalidateSwapchainResources() {
    std::shared_ptr<ColorBlit> blit;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        blit = std::move(g_layerBlit);
        g_layer.Reset();
        g_layerRtvHeap.Reset();
        g_layerRtv = {};
        g_layerDesc = {};
        g_layerHasFrame = false;
    }
    if (blit) blit->Shutdown();
}
