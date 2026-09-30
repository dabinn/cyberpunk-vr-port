#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

struct DesktopCoverTransform {
    double sourceX = 0.0;
    double sourceY = 0.0;
    double sourceWidth = 0.0;
    double sourceHeight = 0.0;
    UINT destinationWidth = 0;
    UINT destinationHeight = 0;
};

bool ComputeDesktopCoverTransform(
    UINT sourceWidth, UINT sourceHeight,
    UINT destinationWidth, UINT destinationHeight,
    DesktopCoverTransform& out);

bool GetDesktopPhysicalClientRect(HWND hwnd, RECT& clientRect, POINT& clientOrigin);
bool GetDesktopMonitorWindowRect(
    HWND hwnd, RECT& windowRect,
    RECT* monitorRect = nullptr,
    RECT* targetClientRect = nullptr);

void DesktopMirrorSetDeviceAndQueue(ID3D12Device* device, ID3D12CommandQueue* queue);
void DesktopMirrorRender(IDXGISwapChain* swapChain, HWND hwnd);
void DesktopMirrorInvalidate();
