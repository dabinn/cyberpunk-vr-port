#pragma once
#include <d3d12.h>
#include <wrl/client.h>

namespace cvr::stereo {
// Array output for a common geometry pass, with an explicit copy back into the
// native single-view targets consumed by unmodified lighting/temporal shaders.
// Not connected to game rendering yet. The owner retains this object and native
// destinations until their command queue fence completes; no CPU waits here.
class StereoTargetArray {
public:
    HRESULT Initialize(ID3D12Device*,const D3D12_RESOURCE_DESC& nativeTarget,
        D3D12_RESOURCE_STATES initialState,const D3D12_CLEAR_VALUE* clear=nullptr);
    HRESULT CopyEye(ID3D12GraphicsCommandList*,UINT eye,D3D12_RESOURCE_STATES sourceState,
        ID3D12Resource* nativeTarget,D3D12_RESOURCE_STATES targetBefore,D3D12_RESOURCE_STATES targetAfter) const;
    HRESULT ImportEye(ID3D12GraphicsCommandList*,UINT eye,D3D12_RESOURCE_STATES arrayState,
        ID3D12Resource* nativeSource,D3D12_RESOURCE_STATES sourceState) const;
    ID3D12Resource* Resource() const {return resource_.Get();}
    UINT PlaneCount() const {return planes_;}
private:
    Microsoft::WRL::ComPtr<ID3D12Resource> resource_;
    D3D12_RESOURCE_DESC native_{};
    UINT planes_{};
};
}
