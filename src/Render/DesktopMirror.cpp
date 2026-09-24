#include "Render/DesktopMirror.hpp"

#include "Render/ColorBlit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>
#include <wrl/client.h>

extern void Log(const char* fmt, ...);
extern "C" UINT GetForcedDisplayModeWidth();
extern "C" UINT GetForcedDisplayModeHeight();
extern "C" UINT GetForcedWindowWidth();

namespace {
using Microsoft::WRL::ComPtr;

using GetClientRectFn = BOOL(WINAPI*)(HWND, LPRECT);
using SetWindowPosFn = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);

GetClientRectFn OriginalGetClientRect() {
    static auto fn = reinterpret_cast<GetClientRectFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetClientRect"));
    return fn;
}

SetWindowPosFn OriginalSetWindowPos() {
    static auto fn = reinterpret_cast<SetWindowPosFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowPos"));
    return fn;
}

bool UsesAutomaticDesktopWindow() {
    const UINT windowWidth = GetForcedWindowWidth();
    const UINT renderWidth = GetForcedDisplayModeWidth();
    return windowWidth == 0 || windowWidth == renderWidth;
}

bool UpdateWindowForCurrentMonitor(HWND hwnd) {
    if (!hwnd || !UsesAutomaticDesktopWindow()) return false;

    RECT outer{};
    RECT monitorRect{};
    RECT targetClientRect{};
    if (!GetDesktopMonitorWindowRect(hwnd, outer, &monitorRect, &targetClientRect)) return false;

    RECT client{};
    POINT origin{};
    if (GetDesktopPhysicalClientRect(hwnd, client, origin)) {
        const int clientWidth = client.right - client.left;
        const int clientHeight = client.bottom - client.top;
        const int targetWidth = targetClientRect.right - targetClientRect.left;
        const int targetHeight = targetClientRect.bottom - targetClientRect.top;
        if (origin.x == targetClientRect.left && origin.y == targetClientRect.top &&
            clientWidth == targetWidth && clientHeight == targetHeight) {
            return true;
        }
    }

    const int width = outer.right - outer.left;
    const int height = outer.bottom - outer.top;
    const auto setWindowPos = OriginalSetWindowPos();
    if (!setWindowPos || !setWindowPos(hwnd, nullptr, outer.left, outer.top, width, height,
                                       SWP_NOZORDER | SWP_NOACTIVATE)) {
        return false;
    }

    Log("DesktopMirror: monitor client updated to (%ld,%ld)-(%ld,%ld) within monitor (%ld,%ld)-(%ld,%ld)\n",
        targetClientRect.left, targetClientRect.top,
        targetClientRect.right, targetClientRect.bottom,
        monitorRect.left, monitorRect.top, monitorRect.right, monitorRect.bottom);
    return true;
}

struct FrameContext {
    ComPtr<ID3D12CommandAllocator> allocator;
    uint64_t fenceValue = 0;
};

struct DesktopMirrorState {
    std::mutex mutex;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D12Resource> sourceCopy;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    uint64_t nextFenceValue = 1;
    std::vector<FrameContext> frames;
    ColorBlit blit;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

DesktopMirrorState g_state;

bool WaitForFenceLocked(uint64_t value) {
    if (!value || !g_state.fence || g_state.fence->GetCompletedValue() >= value) return true;
    if (!g_state.fenceEvent || FAILED(g_state.fence->SetEventOnCompletion(value, g_state.fenceEvent))) {
        return false;
    }
    return WaitForSingleObject(g_state.fenceEvent, 1000) == WAIT_OBJECT_0;
}

void ResetResourcesLocked() {
    uint64_t latestFence = 0;
    for (const auto& frame : g_state.frames) latestFence = (std::max)(latestFence, frame.fenceValue);
    WaitForFenceLocked(latestFence);
    g_state.frames.clear();
    g_state.commandList.Reset();
    g_state.sourceCopy.Reset();
    g_state.fence.Reset();
    if (g_state.fenceEvent) {
        CloseHandle(g_state.fenceEvent);
        g_state.fenceEvent = nullptr;
    }
    g_state.blit.Shutdown();
    g_state.nextFenceValue = 1;
    g_state.width = 0;
    g_state.height = 0;
    g_state.format = DXGI_FORMAT_UNKNOWN;
}

bool EnsureResourcesLocked(IDXGISwapChain3* swapChain, ID3D12Resource* backBuffer) {
    if (!swapChain || !backBuffer || !g_state.device || !g_state.queue) return false;

    DXGI_SWAP_CHAIN_DESC swapDesc{};
    if (FAILED(swapChain->GetDesc(&swapDesc)) || swapDesc.BufferCount == 0) return false;
    const D3D12_RESOURCE_DESC resourceDesc = backBuffer->GetDesc();
    const UINT width = static_cast<UINT>(resourceDesc.Width);
    const UINT height = resourceDesc.Height;
    const DXGI_FORMAT format = swapDesc.BufferDesc.Format;
    if (g_state.commandList && g_state.sourceCopy &&
        g_state.width == width && g_state.height == height && g_state.format == format &&
        g_state.frames.size() == swapDesc.BufferCount) {
        return true;
    }

    ResetResourcesLocked();
    g_state.width = width;
    g_state.height = height;
    g_state.format = format;

    D3D12_RESOURCE_DESC copyDesc = resourceDesc;
    copyDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(g_state.device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &copyDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&g_state.sourceCopy)))) {
        Log("DesktopMirror: failed to create source copy %ux%u format=%u\n",
            width, height, static_cast<unsigned>(format));
        ResetResourcesLocked();
        return false;
    }

    g_state.frames.resize(swapDesc.BufferCount);
    for (auto& frame : g_state.frames) {
        if (FAILED(g_state.device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator)))) {
            ResetResourcesLocked();
            return false;
        }
    }
    if (FAILED(g_state.device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_state.frames[0].allocator.Get(), nullptr,
            IID_PPV_ARGS(&g_state.commandList))) ||
        FAILED(g_state.commandList->Close()) ||
        FAILED(g_state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_state.fence)))) {
        ResetResourcesLocked();
        return false;
    }
    g_state.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_state.fenceEvent || !g_state.blit.EnsureInitialized(
            g_state.device.Get(), format, width, height)) {
        ResetResourcesLocked();
        return false;
    }

    Log("DesktopMirror: initialized center-cover pass %ux%u format=%u buffers=%u\n",
        width, height, static_cast<unsigned>(format), swapDesc.BufferCount);
    return true;
}
} // namespace

bool ComputeDesktopCoverTransform(
        UINT sourceWidth, UINT sourceHeight,
        UINT destinationWidth, UINT destinationHeight,
        DesktopCoverTransform& out) {
    out = {};
    if (!sourceWidth || !sourceHeight || !destinationWidth || !destinationHeight) return false;

    out.sourceWidth = static_cast<double>(sourceWidth);
    out.sourceHeight = static_cast<double>(sourceHeight);
    out.destinationWidth = destinationWidth;
    out.destinationHeight = destinationHeight;

    const double sourceAspect = static_cast<double>(sourceWidth) / sourceHeight;
    const double destinationAspect = static_cast<double>(destinationWidth) / destinationHeight;
    if (sourceAspect < destinationAspect) {
        out.sourceHeight = static_cast<double>(sourceWidth) / destinationAspect;
        out.sourceY = (static_cast<double>(sourceHeight) - out.sourceHeight) * 0.5;
    } else if (sourceAspect > destinationAspect) {
        out.sourceWidth = static_cast<double>(sourceHeight) * destinationAspect;
        out.sourceX = (static_cast<double>(sourceWidth) - out.sourceWidth) * 0.5;
    }
    return true;
}

bool GetDesktopPhysicalClientRect(HWND hwnd, RECT& clientRect, POINT& clientOrigin) {
    clientRect = {};
    clientOrigin = {};
    const auto getClientRect = OriginalGetClientRect();
    if (!hwnd || !getClientRect || !getClientRect(hwnd, &clientRect)) return false;
    return ClientToScreen(hwnd, &clientOrigin) != FALSE;
}

bool GetDesktopMonitorWindowRect(
        HWND hwnd, RECT& windowRect, RECT* monitorRect, RECT* targetClientRect) {
    windowRect = {};
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    if (!hwnd || !monitor || !GetMonitorInfoW(monitor, &mi)) return false;

    const UINT monitorWidth = static_cast<UINT>(mi.rcMonitor.right - mi.rcMonitor.left);
    const UINT monitorHeight = static_cast<UINT>(mi.rcMonitor.bottom - mi.rcMonitor.top);
    const UINT sourceWidth = GetForcedDisplayModeWidth();
    const UINT sourceHeight = GetForcedDisplayModeHeight();
    if (!monitorWidth || !monitorHeight || !sourceWidth || !sourceHeight) return false;

    // Do not upscale a narrow VR render just to fill the desktop monitor. When the render is
    // already at least as wide as the monitor, keep the full-monitor presentation and crop the
    // tall source to the monitor aspect. Otherwise preserve source pixels 1:1 horizontally and
    // crop only as much height as the current monitor can show.
    const UINT clientWidth = (std::min)(sourceWidth, monitorWidth);
    const UINT clientHeight = sourceWidth < monitorWidth
        ? (std::min)(sourceHeight, monitorHeight)
        : monitorHeight;
    const LONG clientLeft = mi.rcMonitor.left +
        (static_cast<LONG>(monitorWidth) - static_cast<LONG>(clientWidth)) / 2;
    const LONG clientTop = mi.rcMonitor.top +
        (static_cast<LONG>(monitorHeight) - static_cast<LONG>(clientHeight)) / 2;
    const RECT desiredClient{
        clientLeft,
        clientTop,
        clientLeft + static_cast<LONG>(clientWidth),
        clientTop + static_cast<LONG>(clientHeight)};

    RECT outer{0, 0, static_cast<LONG>(clientWidth), static_cast<LONG>(clientHeight)};
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    if (!AdjustWindowRectEx(&outer, style, GetMenu(hwnd) != nullptr, exStyle)) return false;

    windowRect.left = desiredClient.left + outer.left;
    windowRect.top = desiredClient.top + outer.top;
    windowRect.right = windowRect.left + (outer.right - outer.left);
    windowRect.bottom = windowRect.top + (outer.bottom - outer.top);
    if (monitorRect) *monitorRect = mi.rcMonitor;
    if (targetClientRect) *targetClientRect = desiredClient;
    return true;
}

void DesktopMirrorSetDeviceAndQueue(ID3D12Device* device, ID3D12CommandQueue* queue) {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    if (g_state.device.Get() == device && g_state.queue.Get() == queue) return;
    ResetResourcesLocked();
    g_state.device = device;
    g_state.queue = queue;
}

void DesktopMirrorInvalidate() {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    ResetResourcesLocked();
}

void DesktopMirrorRender(IDXGISwapChain* swapChain, HWND hwnd) {
    if (!swapChain || !hwnd) return;
    UpdateWindowForCurrentMonitor(hwnd);

    RECT client{};
    POINT origin{};
    if (!GetDesktopPhysicalClientRect(hwnd, client, origin)) return;
    const UINT destinationWidth = static_cast<UINT>((std::max)(0L, client.right - client.left));
    const UINT destinationHeight = static_cast<UINT>((std::max)(0L, client.bottom - client.top));
    if (!destinationWidth || !destinationHeight) return;

    ComPtr<IDXGISwapChain3> swapChain3;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))) || !swapChain3) return;
    const UINT frameIndex = swapChain3->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> backBuffer;
    if (FAILED(swapChain3->GetBuffer(frameIndex, IID_PPV_ARGS(&backBuffer))) || !backBuffer) return;

    const D3D12_RESOURCE_DESC backDesc = backBuffer->GetDesc();
    DesktopCoverTransform transform{};
    if (!ComputeDesktopCoverTransform(
            static_cast<UINT>(backDesc.Width), backDesc.Height,
            destinationWidth, destinationHeight, transform)) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_state.mutex);
    if (!EnsureResourcesLocked(swapChain3.Get(), backBuffer.Get()) || frameIndex >= g_state.frames.size()) return;
    FrameContext& frame = g_state.frames[frameIndex];
    if (!WaitForFenceLocked(frame.fenceValue) || FAILED(frame.allocator->Reset()) ||
        FAILED(g_state.commandList->Reset(frame.allocator.Get(), nullptr))) {
        return;
    }

    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = backBuffer.Get();
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = g_state.sourceCopy.Get();
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_state.commandList->ResourceBarrier(2, barriers);
    g_state.commandList->CopyResource(g_state.sourceCopy.Get(), backBuffer.Get());

    std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    std::swap(barriers[1].Transition.StateBefore, barriers[1].Transition.StateAfter);
    g_state.commandList->ResourceBarrier(2, barriers);

    const float u0 = static_cast<float>(transform.sourceX / backDesc.Width);
    const float v0 = static_cast<float>(transform.sourceY / backDesc.Height);
    const float u1 = static_cast<float>((transform.sourceX + transform.sourceWidth) / backDesc.Width);
    const float v1 = static_cast<float>((transform.sourceY + transform.sourceHeight) / backDesc.Height);
    if (!g_state.blit.RecordBlit(
            g_state.commandList.Get(), g_state.sourceCopy.Get(), backBuffer.Get(), u0, v0, u1, v1)) {
        g_state.commandList->Close();
        return;
    }

    D3D12_RESOURCE_BARRIER toPresent{};
    toPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toPresent.Transition.pResource = backBuffer.Get();
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_state.commandList->ResourceBarrier(1, &toPresent);
    if (FAILED(g_state.commandList->Close())) return;

    ID3D12CommandList* lists[] = {g_state.commandList.Get()};
    g_state.queue->ExecuteCommandLists(1, lists);
    const uint64_t fenceValue = g_state.nextFenceValue++;
    if (SUCCEEDED(g_state.queue->Signal(g_state.fence.Get(), fenceValue))) {
        frame.fenceValue = fenceValue;
    }
}
