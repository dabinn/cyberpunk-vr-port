#include "Render/StereoSceneState.hpp"
#include "Render/RenderProbeScope.hpp"
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <cstdio>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
using namespace cvr::stereo::scene_state;
namespace cvr::stereo::native_probe {bool Enabled(){return true;}}
extern "C" {__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_NativeStereoProbeState{1};}
static void Check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("D3D12 failed");}
static void Require(bool v){if(!v)throw std::runtime_error("Scene binding/descriptor validation failed");}
int main() try {
    auto* list=reinterpret_cast<ID3D12GraphicsCommandList*>(0x1000);auto* root=reinterpret_cast<ID3D12RootSignature*>(0x2000);
    auto* heap=reinterpret_cast<ID3D12DescriptorHeap*>(0x3000);Bindings snapshot;
    D3D12_DESCRIPTOR_RANGE ranges[36]{};D3D12_ROOT_PARAMETER params[36]{};
    for(UINT i=0;i<36;++i){ranges[i]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,i,0,0};params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[i].DescriptorTable={1,&ranges[i]};}
    D3D12_ROOT_SIGNATURE_DESC desc{36,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> blob,error;
    Check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));RootCreated(root,blob->GetBufferPointer(),blob->GetBufferSize());
    Reset(list,true);Root(list,root);Topology(list,D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);Heaps(list,1,&heap);
    for(UINT i=0;i<32;++i)Table(list,i,{0x4000+i*32});Require(Snapshot(list,snapshot) && snapshot.tableMask==0xffffffff);
    Table(list,36,{0xBAD});Require(!Snapshot(list,snapshot));
    Reset(list,true);Root(list,root);Topology(list,D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);Heaps(list,1,&heap);
    for(UINT i=0;i<32;++i)Table(list,i,{0x4000+i*32});
    for(UINT i=32;i<36;++i)Table(list,i,{0x4000+i*32});Require(Snapshot(list,snapshot));Require(snapshot.tables[35].ptr==0x4000+35*32 && snapshot.tableMask==0xfffffffff);
    ComputeRoot(list,root);ComputeTable(list,35,{0xABCD});Require(Snapshot(list,snapshot) && snapshot.computeTables[35].ptr==0xABCD);
    const D3D12_SHADING_RATE_COMBINER combiners[]{D3D12_SHADING_RATE_COMBINER_MIN,D3D12_SHADING_RATE_COMBINER_OVERRIDE};
    ShadingRate(list,D3D12_SHADING_RATE_2X2,combiners);ShadingRateImage(list,reinterpret_cast<ID3D12Resource*>(0x7770));
    {cvr::stereo::probe::Scope internal;ShadingRate(list,D3D12_SHADING_RATE_1X1,nullptr);ShadingRateImage(list,nullptr);}
    Require(Snapshot(list,snapshot) && snapshot.rate.known && snapshot.rate.value==D3D12_SHADING_RATE_2X2 && snapshot.rate.combiners[1]==D3D12_SHADING_RATE_COMBINER_OVERRIDE && snapshot.rate.image==reinterpret_cast<ID3D12Resource*>(0x7770));
    {cvr::stereo::probe::Scope internal;Table(list,35,{0xBAD});Stencil(list,255);}
    Require(Snapshot(list,snapshot) && snapshot.tables[35].ptr!=0xBAD && snapshot.stencil==0);
    Heaps(list,1,&heap);Require(Snapshot(list,snapshot) && snapshot.computeTableMask==(uint64_t(1)<<35));heap=reinterpret_cast<ID3D12DescriptorHeap*>(0x5000);Heaps(list,1,&heap);Require(Snapshot(list,snapshot) && snapshot.tableMask==0 && snapshot.computeTableMask==0);
    for(UINT i=0;i<36;++i)Table(list,i,{0x6000+i*32});Stencil(list,255);Require(!Snapshot(list,snapshot));Stencil(list,7);Require(Snapshot(list,snapshot) && snapshot.stencil==7);
    Require(snapshot.computeTableMask==0);
    Reset(list);Require(!Snapshot(list,snapshot));ResetCapture();Require(!Snapshot(list,snapshot));
    ComPtr<ID3D12Debug> debug;Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));debug->EnableDebugLayer();ComPtr<IDXGIFactory6> factory;Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));ComPtr<ID3D12Device> device;Check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    std::array<ComPtr<ID3D12Resource>,4> resources;D3D12_RESOURCE_DESC r{};r.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;r.Width=129;r.Height=97;r.DepthOrArraySize=r.MipLevels=r.SampleDesc.Count=1;
    D3D12_HEAP_PROPERTIES memory{};memory.Type=D3D12_HEAP_TYPE_DEFAULT;
    cvr::stereo::native_probe::DrawRecord draw;draw.rtCount=3;draw.viewport={0,0,129,97,0,1};draw.depth=0x9000;
    for(UINT i=0;i<4;++i){r.Format=DXGI_FORMAT(i<2?24:i==2?28:20);r.Flags=i==3?D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL:D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        Check(device->CreateCommittedResource(&memory,D3D12_HEAP_FLAG_NONE,&r,i==3?D3D12_RESOURCE_STATE_DEPTH_WRITE:D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&resources[i])));
        if(i==3)DsvCreated(resources[i].Get(),nullptr,{draw.depth});else {draw.targets[i]=0x8000+i*32;RtvCreated(resources[i].Get(),nullptr,{draw.targets[i]});}}
    Targets captured;Require(CaptureTargets(draw,captured));for(UINT i=0;i<4;++i)Require(captured.resources[i].Get()==resources[i].Get());
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};dsv.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;dsv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;dsv.Flags=D3D12_DSV_FLAG_READ_ONLY_DEPTH;
    DsvCreated(resources[3].Get(),&dsv,{draw.depth});Require(!CaptureTargets(draw,captured));DsvCreated(resources[3].Get(),nullptr,{draw.depth});
    RtvCreated(nullptr,nullptr,{draw.targets[1]});Require(!CaptureTargets(draw,captured));RtvCreated(resources[1].Get(),nullptr,{draw.targets[1]});
    draw.viewport.Width=128;Require(!CaptureTargets(draw,captured));draw.viewport.Width=129;draw.rtContiguous=TRUE;Require(!CaptureTargets(draw,captured));draw.rtContiguous=FALSE;Require(CaptureTargets(draw,captured));
    std::puts("PASS 36 root tables, heap invalidation, internal-command isolation, reset, descriptor overwrite, writable depth and exact target dimensions");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
