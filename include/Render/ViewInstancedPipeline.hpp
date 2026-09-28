#pragma once
#include <d3d12.h>
#include <array>
#include <vector>
#include <cstdint>

namespace cvr::stereo {
// Caller supplies validated view-aware shaders and matching array RTV/DSV views.
// This only creates a PSO; it does not enable or redirect game rendering.
struct ViewInstancedLayout {
    std::array<D3D12_VIEW_INSTANCE_LOCATION,2> locations{{{0,0},{0,1}}};
    bool allowMasking=false;
};
HRESULT CreateViewInstancedPipeline(ID3D12Device2*,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC&,const ViewInstancedLayout&,
    ID3D12PipelineState**);
struct PipelineStreamInfo {
    D3D12_SHADER_BYTECODE vs{},ps{};
    ID3D12RootSignature* root{};
    D3D12_RT_FORMAT_ARRAY targets{};
    DXGI_FORMAT depthFormat=DXGI_FORMAT_UNKNOWN;
    D3D12_BLEND_DESC blend{};
    D3D12_DEPTH_STENCIL_DESC depth{};
    DXGI_SAMPLE_DESC samples{};
    bool haveBlend{},haveDepth{},depthBounds{};
};
// Pointers remain borrowed from the caller during these synchronous operations.
// Scalar payloads start at +4, pointer-aligned payloads at +8 on x64.
bool ReadPipelineStream(const D3D12_PIPELINE_STATE_STREAM_DESC&,PipelineStreamInfo&);
bool BuildViewInstancedStream(const D3D12_PIPELINE_STATE_STREAM_DESC&,const D3D12_SHADER_BYTECODE& replacementVs,
    const ViewInstancedLayout&,std::vector<uint8_t>&,const D3D12_SHADER_BYTECODE& replacementPs={});
HRESULT CreateViewInstancedPipelineStream(ID3D12Device2*,const D3D12_PIPELINE_STATE_STREAM_DESC&,
    const D3D12_SHADER_BYTECODE& replacementVs,const ViewInstancedLayout&,ID3D12PipelineState**,const D3D12_SHADER_BYTECODE& replacementPs={});
}
