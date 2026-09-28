#include "Render/ViewInstancedPipeline.hpp"
#include <cstdio>
#include <cstring>
#include <stdexcept>
using namespace cvr::stereo;
template<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE T,class V> struct alignas(void*) Part {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE tag=T;V value{};};
static void Require(bool v){if(!v)throw std::runtime_error("Pipeline stream assertion failed");}
int main() try {
    struct Source {
        Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK,UINT> mask;
        Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS,D3D12_SHADER_BYTECODE> vs;
        Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT,DXGI_FORMAT> depth;
        Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS,D3D12_SHADER_BYTECODE> ps;
        Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO,D3D12_CACHED_PIPELINE_STATE> cache;
    } source;
    const uint8_t original[8]{1},replacement[16]{2},pixel[8]{3},cached[16]{4};
    source.mask.value=0xF0F0F0F0;source.vs.value={original,sizeof(original)};source.depth.value=DXGI_FORMAT_D32_FLOAT;
    source.ps.value={pixel,sizeof(pixel)};source.cache.value={cached,sizeof(cached)};
    static_assert(offsetof(decltype(source.mask),value)==4);static_assert(offsetof(decltype(source.vs),value)==8);
    const auto unchanged=source;D3D12_PIPELINE_STATE_STREAM_DESC input{sizeof(source),&source};
    PipelineStreamInfo info;Require(ReadPipelineStream(input,info));Require(info.vs.pShaderBytecode==original && info.ps.pShaderBytecode==pixel && info.depthFormat==DXGI_FORMAT_D32_FLOAT);
    ViewInstancedLayout layout;std::vector<uint8_t> output;Require(BuildViewInstancedStream(input,{replacement,sizeof(replacement)},layout,output));
    D3D12_PIPELINE_STATE_STREAM_DESC changed{output.size(),output.data()};Require(ReadPipelineStream(changed,info));Require(info.vs.pShaderBytecode==replacement && info.ps.pShaderBytecode==pixel);
    Require(std::memcmp(&source,&unchanged,sizeof(source))==0);
    D3D12_CACHED_PIPELINE_STATE newCache{};std::memcpy(&newCache,output.data()+offsetof(Source,cache)+offsetof(decltype(source.cache),value),sizeof(newCache));
    Require(!newCache.pCachedBlob && !newCache.CachedBlobSizeInBytes);
    std::vector<uint8_t> linked;
    Require(BuildViewInstancedStream(input,{replacement,sizeof(replacement)},layout,linked,{replacement,sizeof(replacement)}));
    D3D12_PIPELINE_STATE_STREAM_DESC linkedStream{linked.size(),linked.data()};Require(ReadPipelineStream(linkedStream,info));
    Require(info.ps.pShaderBytecode==replacement);
    const auto valid=output;std::vector<uint8_t> rejected{7};
    Require(!BuildViewInstancedStream(changed,{replacement,sizeof(replacement)},layout,rejected) && rejected==std::vector<uint8_t>{7});
    for(size_t len=1;len<sizeof(source.mask)+sizeof(source.vs);++len) {
        if(len==sizeof(source.mask))continue;
        D3D12_PIPELINE_STATE_STREAM_DESC truncated{len,&source};Require(!ReadPipelineStream(truncated,info));
    }
    source.depth.tag=D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS;
    Require(!ReadPipelineStream(input,info));
    source.depth.tag=static_cast<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE>(999);
    Require(!BuildViewInstancedStream(input,{replacement,sizeof(replacement)},layout,rejected));
    std::puts("PASS scalar/pointer alignment, unchanged PS/state/source, cache removal, malformed/duplicate tags and already-instanced rejection");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
