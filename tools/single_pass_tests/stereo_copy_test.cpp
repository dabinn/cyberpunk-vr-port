#include "Render/StereoTargetArray.hpp"
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <array>
#include <vector>
#include <cstdio>
#include <cstring>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr,const char* text) {if(FAILED(hr)){std::fprintf(stderr,"%s: %08X\n",text,unsigned(hr));throw std::runtime_error(text);}}
static void Require(bool value,const char* text) {if(!value)throw std::runtime_error(text);}
static ComPtr<ID3D12Resource> Readback(ID3D12Device* device,UINT64 size) {
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=size;
    d.Height=d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES h{};h.Type=D3D12_HEAP_TYPE_READBACK;ComPtr<ID3D12Resource> result;
    Check(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&result)),"Readback");return result;
}
static void Barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    if(a==b)return;D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};list->ResourceBarrier(1,&x);
}
static void ToReadback(ID3D12GraphicsCommandList* list,ID3D12Resource* src,UINT subresource,
    ID3D12Resource* dst,const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint) {
    D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=src;a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;a.SubresourceIndex=subresource;
    b.pResource=dst;b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint=footprint;list->CopyTextureRegion(&b,0,0,0,&a,nullptr);
}
static void Validate(ID3D12Device* device) {
    ComPtr<ID3D12InfoQueue> info;Check(device->QueryInterface(IID_PPV_ARGS(&info)),"InfoQueue");
    for(UINT64 i=0;i<info->GetNumStoredMessages();++i) {
        SIZE_T size{};info->GetMessage(i,nullptr,&size);std::vector<char> data(size);
        auto* m=reinterpret_cast<D3D12_MESSAGE*>(data.data());info->GetMessage(i,m,&size);
        if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::fprintf(stderr,"%s\n",m->pDescription);throw std::runtime_error("D3D12 validation error");}
    }
}
int main(int argc,char** argv) try {
    const bool hardware=argc>1 && std::strcmp(argv[1],"--hardware")==0;
    ComPtr<ID3D12Debug> debug;Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"Debug layer");debug->EnableDebugLayer();
    ComPtr<IDXGIFactory6> factory;Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Factory");
    ComPtr<IDXGIAdapter1> adapter;
    Check(hardware?factory->EnumAdapterByGpuPreference(0,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter)):
        factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)),"Adapter");
    ComPtr<ID3D12Device> device;Check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"Device");
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"Queue");
    ComPtr<ID3D12Fence> fence;Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Fence");
    UINT64 serial{},comparisons{},bytesCompared{};
    const DXGI_FORMAT formats[]={DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_R32G8X24_TYPELESS};
    for(auto format:formats)for(UINT width:{67u,129u}) {
        const UINT height=width/2+5;const bool depth=format==DXGI_FORMAT_R32G8X24_TYPELESS;
        const auto state=depth?D3D12_RESOURCE_STATE_DEPTH_WRITE:D3D12_RESOURCE_STATE_RENDER_TARGET;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width;desc.Height=height;
        desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Format=format;
        desc.Flags=depth?D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL:D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{};clear.Format=depth?DXGI_FORMAT_D32_FLOAT_S8X24_UINT:format;
        cvr::stereo::StereoTargetArray pair;Check(pair.Initialize(device.Get(),desc,state,&clear),"Stereo array");
        Require(pair.PlaneCount()==(depth?2u:1u),"Unexpected format planes");
        ComPtr<ID3D12Resource> native;D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_SOURCE,&clear,IID_PPV_ARGS(&native)),"Native 2D destination");
        const auto arrayDesc=pair.Resource()->GetDesc();const UINT subresources=pair.PlaneCount()*2;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);std::vector<UINT> rows(subresources);std::vector<UINT64> rowBytes(subresources);UINT64 bytes{};
        device->GetCopyableFootprints(&arrayDesc,0,subresources,0,footprints.data(),rows.data(),rowBytes.data(),&bytes);
        auto reference=Readback(device.Get(),bytes),actual=Readback(device.Get(),bytes);
        ComPtr<ID3D12DescriptorHeap> views;D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.NumDescriptors=2;hd.Type=depth?D3D12_DESCRIPTOR_HEAP_TYPE_DSV:D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&views)),"View heap");
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE,2> handles{};
        for(UINT eye=0;eye<2;++eye) {
            auto handle=views->GetCPUDescriptorHandleForHeapStart();handle.ptr+=eye*device->GetDescriptorHandleIncrementSize(hd.Type);handles[eye]=handle;
            if(depth){D3D12_DEPTH_STENCIL_VIEW_DESC v{};v.Format=clear.Format;v.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2DARRAY;v.Texture2DArray.FirstArraySlice=eye;v.Texture2DArray.ArraySize=1;device->CreateDepthStencilView(pair.Resource(),&v,handle);}
            else {D3D12_RENDER_TARGET_VIEW_DESC v{};v.Format=format;v.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2DARRAY;v.Texture2DArray.FirstArraySlice=eye;v.Texture2DArray.ArraySize=1;device->CreateRenderTargetView(pair.Resource(),&v,handle);}
        }
        // Reuse the same destinations to catch state-restoration errors.
        for(UINT cycle=0;cycle<2;++cycle) {
            ComPtr<ID3D12CommandAllocator> allocator;Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"Allocator");
            ComPtr<ID3D12GraphicsCommandList> list;Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"Commands");
            const D3D12_RECT region{7,3,LONG(width-9),LONG(height-4)};
            for(UINT eye=0;eye<2;++eye) {
                if(depth){const auto flags=D3D12_CLEAR_FLAGS(D3D12_CLEAR_FLAG_DEPTH|D3D12_CLEAR_FLAG_STENCIL);
                    list->ClearDepthStencilView(handles[eye],flags,.2f+.5f*eye,UINT8(17+63*eye+cycle),0,nullptr);
                    list->ClearDepthStencilView(handles[eye],flags,.3f+.4f*eye,UINT8(33+49*eye+cycle),1,&region);}
                else {const float color[4]{.1f+.6f*eye,.25f+.1f*cycle,.5f,.75f};
                    const float inner[4]{.5f,.1f+.7f*eye,.2f+.1f*cycle,1};
                    list->ClearRenderTargetView(handles[eye],color,0,nullptr);list->ClearRenderTargetView(handles[eye],inner,1,&region);}
            }
            Barrier(list.Get(),pair.Resource(),state,D3D12_RESOURCE_STATE_COPY_SOURCE);
            for(UINT sub=0;sub<subresources;++sub)ToReadback(list.Get(),pair.Resource(),sub,reference.Get(),footprints[sub]);
            Barrier(list.Get(),pair.Resource(),D3D12_RESOURCE_STATE_COPY_SOURCE,state);
            Require(pair.CopyEye(list.Get(),2,state,native.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE)==E_INVALIDARG,"Invalid eye accepted");
            for(UINT eye=0;eye<2;++eye) {
                Check(pair.CopyEye(list.Get(),eye,state,native.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE),"Copy eye into native 2D target");
                for(UINT plane=0;plane<pair.PlaneCount();++plane)ToReadback(list.Get(),native.Get(),plane,actual.Get(),footprints[eye+plane*2]);
            }
            Check(list->Close(),"Close");ID3D12CommandList* commands[]={list.Get()};queue->ExecuteCommandLists(1,commands);Check(queue->Signal(fence.Get(),++serial),"Signal");
            HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Require(event!=nullptr,"Event");Check(fence->SetEventOnCompletion(serial,event),"Fence completion");
            const DWORD waited=WaitForSingleObject(event,5000);CloseHandle(event);Require(waited==WAIT_OBJECT_0,"GPU timeout");
            void *a{},*b{};D3D12_RANGE range{0,SIZE_T(bytes)};Check(reference->Map(0,&range,&a),"Reference map");Check(actual->Map(0,&range,&b),"Actual map");
            UINT64 different{},eyeDifference{};
            for(UINT sub=0;sub<subresources;++sub)for(UINT row=0;row<rows[sub];++row) {
                const auto offset=footprints[sub].Offset+row*footprints[sub].Footprint.RowPitch;
                const auto* lhs=static_cast<const unsigned char*>(a)+offset;const auto* rhs=static_cast<const unsigned char*>(b)+offset;
                for(UINT64 byte=0;byte<rowBytes[sub];++byte){different+=lhs[byte]!=rhs[byte];++bytesCompared;}
            }
            for(UINT row=0;row<rows[0];++row)for(UINT64 byte=0;byte<rowBytes[0];++byte) {
                const auto left=footprints[0].Offset+row*footprints[0].Footprint.RowPitch+byte;
                const auto right=footprints[1].Offset+row*footprints[1].Footprint.RowPitch+byte;
                eyeDifference+=static_cast<const unsigned char*>(a)[left]!=static_cast<const unsigned char*>(a)[right];
            }
            D3D12_RANGE none{};reference->Unmap(0,&none);actual->Unmap(0,&none);
            Require(eyeDifference>0,"Fixture eyes must differ");Require(different==0,"Copied eye/plane data differs");
            Validate(device.Get());comparisons+=subresources;
        }
    }
    std::printf("PASS %s: %llu eye/plane comparisons, %llu bytes, color/MV/depth/stencil, padded rows and target reuse; no D3D12 validation errors\n",hardware?"hardware":"WARP",comparisons,bytesCompared);return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
