#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
namespace cvr::stereo {
// One diagnostic frame. Owner retains native source images until its fence.
class StereoVrsSnapshot {
public:
    HRESULT Initialize(ID3D12Device*,ID3D12Resource* image);
    HRESULT Capture(ID3D12GraphicsCommandList*,ID3D12Resource* image,UINT eye);
    bool Retain(ID3D12GraphicsCommandList*) const;
    HRESULT Read(uint64_t& differences,std::array<std::array<uint64_t,16>,2>& histogram) const;
    ID3D12Resource* SourceImage() const {return frozen_.Get();}
    UINT Width()const{return UINT(desc_.Width);}UINT Height()const{return desc_.Height;}
private:
    Microsoft::WRL::ComPtr<ID3D12Resource> frozen_,readback_;
    D3D12_RESOURCE_DESC desc_{};
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,2> footprints_{};
    UINT64 bytes_{};
};
}
