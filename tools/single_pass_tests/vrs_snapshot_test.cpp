#include "Render/StereoVrsSnapshot.hpp"
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <vector>
#include <cstring>
#include <cstdio>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("D3D12 failure");}
static void Require(bool v){if(!v)throw std::runtime_error("VRS snapshot mismatch");}
int main() try {
    ComPtr<ID3D12Debug> debug;Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));debug->EnableDebugLayer();
    ComPtr<IDXGIFactory6> factory;Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));ComPtr<IDXGIAdapter1> adapter;Check(factory->EnumAdapterByGpuPreference(0,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> device;Check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)));ComPtr<ID3D12CommandAllocator> allocator;Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList5> list;Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
    constexpr UINT width=17,height=9;ComPtr<ID3D12Resource> images[2],uploads[2];D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=width;td.Height=height;td.DepthOrArraySize=td.MipLevels=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8_UINT;
    for(UINT i=0;i<2;++i){D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&images[i])));
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};device->GetCopyableFootprints(&td,0,1,0,&fp,nullptr,nullptr,&bytes);D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=bd.DepthOrArraySize=bd.MipLevels=bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        heap.Type=D3D12_HEAP_TYPE_UPLOAD;Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&uploads[i])));void* p{};D3D12_RANGE none{};Check(uploads[i]->Map(0,&none,&p));std::memset(p,i?5:0,size_t(bytes));uploads[i]->Unmap(0,nullptr);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=uploads[i].Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=fp;to.pResource=images[i].Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={images[i].Get(),0,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE};list->ResourceBarrier(1,&b);}
    cvr::stereo::StereoVrsSnapshot different,frozen;Check(different.Initialize(device.Get(),images[0].Get()));Check(frozen.Initialize(device.Get(),images[0].Get()));
    list->RSSetShadingRateImage(images[0].Get());Check(different.Capture(list.Get(),images[0].Get(),0));list->RSSetShadingRateImage(images[1].Get());Check(different.Capture(list.Get(),images[1].Get(),1));
    list->RSSetShadingRateImage(images[0].Get());Check(frozen.Capture(list.Get(),images[0].Get(),0));list->RSSetShadingRateImage(different.SourceImage());Check(frozen.Capture(list.Get(),different.SourceImage(),1));
    Check(list->Close());ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));ID3D12CommandList* commands[]{list.Get()};queue->ExecuteCommandLists(1,commands);
    ComPtr<ID3D12Fence> fence;Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));Check(queue->Signal(fence.Get(),1));HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Require(event!=nullptr);Check(fence->SetEventOnCompletion(1,event));const auto waited=WaitForSingleObject(event,5000);CloseHandle(event);Require(waited==WAIT_OBJECT_0);
    uint64_t differences{};std::array<std::array<uint64_t,16>,2> histogram{};Check(different.Read(differences,histogram));Require(differences==width*height && histogram[0][0]==width*height && histogram[1][5]==width*height);
    Check(frozen.Read(differences,histogram));Require(differences==0 && histogram[0][0]==width*height && histogram[1][0]==width*height);
    ComPtr<ID3D12InfoQueue> info;Check(device.As(&info));for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T n{};info->GetMessage(i,nullptr,&n);std::vector<char> data(n);auto* m=reinterpret_cast<D3D12_MESSAGE*>(data.data());info->GetMessage(i,m,&n);if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::fprintf(stderr,"%s\n",m->pDescription);throw std::runtime_error("D3D12 validation error");}}
    std::puts("PASS VRS image capture, preserved source copy, row padding and native state transitions");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
