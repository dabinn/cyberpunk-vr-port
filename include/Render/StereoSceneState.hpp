#pragma once
#include "Render/NativeStereoProbe.hpp"
#include <wrl/client.h>
namespace cvr::stereo::scene_state {
struct Bindings {
    struct Rate {
        D3D12_SHADING_RATE value=D3D12_SHADING_RATE_1X1;
        std::array<D3D12_SHADING_RATE_COMBINER,2> combiners{D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
        ID3D12Resource* image{};
        bool known{};
    } rate;
    ID3D12RootSignature* root{};
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE,64> tables{};
    uint64_t tableMask{};
    ID3D12RootSignature* computeRoot{};
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE,64> computeTables{};
    uint64_t computeTableMask{};
    std::array<ID3D12DescriptorHeap*,2> heaps{};
    UINT heapCount{},stencil{};
    D3D12_PRIMITIVE_TOPOLOGY topology{};
    bool resetSeen{},heapsSeen{};
    void Restore(ID3D12GraphicsCommandList*) const;
};
struct Targets {std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,4> resources;};
void RootCreated(ID3D12RootSignature*,const void*,size_t);
void PipelineCreated(ID3D12PipelineState*,bool resolveCompatible);
void ResetCapture();
void Reset(ID3D12GraphicsCommandList*,bool rateKnown=false);
void Root(ID3D12GraphicsCommandList*,ID3D12RootSignature*);
void Table(ID3D12GraphicsCommandList*,UINT,D3D12_GPU_DESCRIPTOR_HANDLE);
void ComputeRoot(ID3D12GraphicsCommandList*,ID3D12RootSignature*);
void ComputeTable(ID3D12GraphicsCommandList*,UINT,D3D12_GPU_DESCRIPTOR_HANDLE);
void Heaps(ID3D12GraphicsCommandList*,UINT,ID3D12DescriptorHeap* const*);
void Topology(ID3D12GraphicsCommandList*,D3D12_PRIMITIVE_TOPOLOGY);
void Stencil(ID3D12GraphicsCommandList*,UINT);
void ShadingRate(ID3D12GraphicsCommandList*,D3D12_SHADING_RATE,const D3D12_SHADING_RATE_COMBINER*);
void ShadingRateImage(ID3D12GraphicsCommandList*,ID3D12Resource*);
bool Snapshot(ID3D12GraphicsCommandList*,Bindings&);
void RtvCreated(ID3D12Resource*,const D3D12_RENDER_TARGET_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
void DsvCreated(ID3D12Resource*,const D3D12_DEPTH_STENCIL_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
bool CaptureTargets(const native_probe::DrawRecord&,Targets&);
bool Compatible(ID3D12PipelineState*);
}
