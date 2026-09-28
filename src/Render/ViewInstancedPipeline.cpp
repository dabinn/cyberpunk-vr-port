#include "Render/ViewInstancedPipeline.hpp"
#include <cstring>
#include <bitset>
#include <utility>

namespace cvr::stereo {
namespace {
// D3D12's pipeline stream ABI requires pointer-aligned subobjects and padding.
#pragma warning(push)
#pragma warning(disable:4324)
template<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type,class T> struct alignas(void*) Part {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type=Type;
    T value{};
};
struct Stream {
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,ID3D12RootSignature*> root;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS,D3D12_SHADER_BYTECODE> vs;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS,D3D12_SHADER_BYTECODE> ps;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS,D3D12_SHADER_BYTECODE> ds;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS,D3D12_SHADER_BYTECODE> hs;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS,D3D12_SHADER_BYTECODE> gs;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT,D3D12_STREAM_OUTPUT_DESC> streamOutput;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND,D3D12_BLEND_DESC> blend;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK,UINT> sampleMask;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER,D3D12_RASTERIZER_DESC> raster;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL,D3D12_DEPTH_STENCIL_DESC> depth;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT,D3D12_INPUT_LAYOUT_DESC> input;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE,D3D12_INDEX_BUFFER_STRIP_CUT_VALUE> stripCut;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY,D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS,D3D12_RT_FORMAT_ARRAY> targets;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT,DXGI_FORMAT> depthFormat;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC,DXGI_SAMPLE_DESC> samples;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK,UINT> nodeMask;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS,D3D12_PIPELINE_STATE_FLAGS> flags;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING,D3D12_VIEW_INSTANCING_DESC> views;
};
#pragma warning(pop)
struct PartLayout {size_t payload{},bytes{};};
constexpr size_t Align(size_t n,size_t alignment){return (n+alignment-1)&~(alignment-1);}
template<class T> constexpr PartLayout Layout() {
    constexpr auto payload=Align(sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE),alignof(T));
    return {payload,Align(payload+sizeof(T),alignof(void*))};
}
PartLayout Layout(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) {
    switch(type) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:return Layout<ID3D12RootSignature*>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:return Layout<D3D12_SHADER_BYTECODE>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:return Layout<D3D12_STREAM_OUTPUT_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:return Layout<D3D12_BLEND_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:return Layout<UINT>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:return Layout<D3D12_RASTERIZER_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:return Layout<D3D12_DEPTH_STENCIL_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:return Layout<D3D12_DEPTH_STENCIL_DESC1>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:return Layout<D3D12_INPUT_LAYOUT_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:return Layout<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:return Layout<D3D12_PRIMITIVE_TOPOLOGY_TYPE>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:return Layout<D3D12_RT_FORMAT_ARRAY>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:return Layout<DXGI_FORMAT>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:return Layout<DXGI_SAMPLE_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:return Layout<D3D12_CACHED_PIPELINE_STATE>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:return Layout<D3D12_PIPELINE_STATE_FLAGS>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:return Layout<D3D12_VIEW_INSTANCING_DESC>();
    default:return {};
    }
}
template<class Visit> bool Walk(const D3D12_PIPELINE_STATE_STREAM_DESC& source,Visit visit) {
    if(!source.pPipelineStateSubobjectStream || !source.SizeInBytes || source.SizeInBytes>65536)return false;
    const auto* bytes=static_cast<const uint8_t*>(source.pPipelineStateSubobjectStream);
    size_t offset{};std::bitset<64> seen;
    while(offset<source.SizeInBytes) {
        if(source.SizeInBytes-offset<sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE))return false;
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{};std::memcpy(&type,bytes+offset,sizeof(type));
        const auto tag=static_cast<unsigned>(type);if(tag>=seen.size() || seen[tag])return false;seen.set(tag);
        const auto part=Layout(type);if(!part.bytes || part.bytes>source.SizeInBytes-offset)return false;
        if(!visit(type,offset+part.payload,bytes+offset+part.payload))return false;
        offset+=part.bytes;
    }
    return true;
}
}

bool ReadPipelineStream(const D3D12_PIPELINE_STATE_STREAM_DESC& source,PipelineStreamInfo& output) {
    PipelineStreamInfo result;
    const bool valid=Walk(source,[&](auto type,size_t,const uint8_t* value){
        switch(type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:std::memcpy(&result.vs,value,sizeof(result.vs));break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:std::memcpy(&result.ps,value,sizeof(result.ps));break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:std::memcpy(&result.root,value,sizeof(result.root));break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:std::memcpy(&result.targets,value,sizeof(result.targets));break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:std::memcpy(&result.depthFormat,value,sizeof(result.depthFormat));break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:std::memcpy(&result.blend,value,sizeof(result.blend));result.haveBlend=true;break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:std::memcpy(&result.samples,value,sizeof(result.samples));break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:std::memcpy(&result.depth,value,sizeof(result.depth));result.haveDepth=true;break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:{D3D12_DEPTH_STENCIL_DESC1 d{};std::memcpy(&d,value,sizeof(d));
            result.depth={d.DepthEnable,d.DepthWriteMask,d.DepthFunc,d.StencilEnable,d.StencilReadMask,d.StencilWriteMask,d.FrontFace,d.BackFace};result.haveDepth=true;result.depthBounds=d.DepthBoundsTestEnable!=FALSE;break;}
        default:break;
        }return true;
    });
    if(!valid || result.targets.NumRenderTargets>8)return false;output=result;return true;
}
bool BuildViewInstancedStream(const D3D12_PIPELINE_STATE_STREAM_DESC& source,const D3D12_SHADER_BYTECODE& vs,
    const ViewInstancedLayout& layout,std::vector<uint8_t>& output,const D3D12_SHADER_BYTECODE& ps) {
    if(!vs.pShaderBytecode || !vs.BytecodeLength)return false;
    if(bool(ps.pShaderBytecode)!=bool(ps.BytecodeLength))return false;
    std::vector<uint8_t> copy;bool found{},foundPs{};
    const bool valid=Walk(source,[&](auto type,size_t offset,const uint8_t* value){
        if(type==D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING)return false;
        if(copy.empty())copy.assign(static_cast<const uint8_t*>(source.pPipelineStateSubobjectStream),
            static_cast<const uint8_t*>(source.pPipelineStateSubobjectStream)+source.SizeInBytes);
        if(type==D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS){std::memcpy(copy.data()+offset,&vs,sizeof(vs));found=true;}
        if(type==D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS){foundPs=true;if(ps.BytecodeLength)std::memcpy(copy.data()+offset,&ps,sizeof(ps));}
        if(type==D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO){D3D12_CACHED_PIPELINE_STATE empty{};std::memcpy(copy.data()+offset,&empty,sizeof(empty));}
        if(type==D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS || type==D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS || type==D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS){
            D3D12_SHADER_BYTECODE shader{};std::memcpy(&shader,value,sizeof(shader));if(shader.BytecodeLength)return false;
        }
        return true;
    });
    if(!valid || !found || (ps.BytecodeLength && !foundPs))return false;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING,D3D12_VIEW_INSTANCING_DESC> views;
    views.value={2,layout.locations.data(),layout.allowMasking?D3D12_VIEW_INSTANCING_FLAG_ENABLE_VIEW_INSTANCE_MASKING:D3D12_VIEW_INSTANCING_FLAG_NONE};
    const auto* data=reinterpret_cast<const uint8_t*>(&views);copy.insert(copy.end(),data,data+sizeof(views));output=std::move(copy);return true;
}
HRESULT CreateViewInstancedPipelineStream(ID3D12Device2* device,const D3D12_PIPELINE_STATE_STREAM_DESC& source,
    const D3D12_SHADER_BYTECODE& vs,const ViewInstancedLayout& layout,ID3D12PipelineState** result,const D3D12_SHADER_BYTECODE& ps) {
    if(!result)return E_POINTER;*result=nullptr;if(!device)return E_INVALIDARG;
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 options{};
    const auto capability=device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3,&options,sizeof(options));
    if(FAILED(capability))return capability;
    if(options.ViewInstancingTier==D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED)return DXGI_ERROR_UNSUPPORTED;
    std::vector<uint8_t> copy;if(!BuildViewInstancedStream(source,vs,layout,copy,ps))return E_INVALIDARG;
    D3D12_PIPELINE_STATE_STREAM_DESC stream{copy.size(),copy.data()};return device->CreatePipelineState(&stream,IID_PPV_ARGS(result));
}

HRESULT CreateViewInstancedPipeline(ID3D12Device2* device,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC& source,const ViewInstancedLayout& layout,
    ID3D12PipelineState** result) {
    if(!result)return E_POINTER;
    *result=nullptr;
    if(!device || !source.VS.pShaderBytecode || !source.VS.BytecodeLength ||
       source.NumRenderTargets>D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)return E_INVALIDARG;
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 options{};
    const auto capability=device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3,&options,sizeof(options));
    if(FAILED(capability))return capability;
    if(options.ViewInstancingTier==D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED)return DXGI_ERROR_UNSUPPORTED;
    Stream s;
    s.root.value=source.pRootSignature;s.vs.value=source.VS;s.ps.value=source.PS;
    s.ds.value=source.DS;s.hs.value=source.HS;s.gs.value=source.GS;s.streamOutput.value=source.StreamOutput;
    s.blend.value=source.BlendState;s.sampleMask.value=source.SampleMask;
    s.raster.value=source.RasterizerState;s.depth.value=source.DepthStencilState;
    s.input.value=source.InputLayout;s.stripCut.value=source.IBStripCutValue;s.topology.value=source.PrimitiveTopologyType;
    s.targets.value.NumRenderTargets=source.NumRenderTargets;
    for(UINT i=0;i<source.NumRenderTargets;++i)s.targets.value.RTFormats[i]=source.RTVFormats[i];
    s.depthFormat.value=source.DSVFormat;s.samples.value=source.SampleDesc;
    s.nodeMask.value=source.NodeMask;s.flags.value=source.Flags;
    // A cached monoscopic PSO cannot describe the changed shaders/view layout.
    // Every other classic descriptor field is preserved in the stream above.
    D3D12_PIPELINE_STATE_STREAM_DESC stream{offsetof(Stream,views),&s};
    return CreateViewInstancedPipelineStream(device,stream,source.VS,layout,result);
}
}
