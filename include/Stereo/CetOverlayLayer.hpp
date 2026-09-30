#pragma once

#include <d3d12.h>
#include <cstdint>

// CET owns a separate D3D12 ImGui renderer. These helpers identify the objects created by
// cyber_engine_tweaks.asi, redirect its main-backbuffer pass into a transparent layer, and replay
// that layer into our MAIN and VRCAM targets without touching CET's private ImGui context.
void CetOverlayNoteCommandListCreated(ID3D12GraphicsCommandList* list, const void* returnAddress);
void CetOverlayNoteRtvCreated(ID3D12Device* device, ID3D12Resource* resource,
                              D3D12_CPU_DESCRIPTOR_HANDLE handle, const void* returnAddress);
bool CetOverlayOwnsCommandList(ID3D12GraphicsCommandList* list);

bool CetOverlayRewriteBarriers(ID3D12GraphicsCommandList* list, UINT count,
                               const D3D12_RESOURCE_BARRIER* barriers,
                               D3D12_RESOURCE_BARRIER* rewritten);

bool CetOverlayRewriteRenderTargets(ID3D12GraphicsCommandList* list, UINT count,
                                    const D3D12_CPU_DESCRIPTOR_HANDLE* handles, BOOL contiguous,
                                    D3D12_CPU_DESCRIPTOR_HANDLE* rewritten,
                                    BOOL* rewrittenContiguous);

bool CetOverlayRecordIntoTarget(ID3D12GraphicsCommandList* list, ID3D12Resource* target,
                                float shiftPx = 0.0f);

void CetOverlayInvalidateSwapchainResources();

extern "C" __declspec(dllexport) int32_t  CyberpunkVR_CetStereoOverlay;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCetCommandLists;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCetRtvs;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCetRedirectBarriers;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCetRedirectBinds;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCetMainComposites;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCetEyeComposites;
