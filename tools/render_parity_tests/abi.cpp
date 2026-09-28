#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
struct Registers {std::array<uint64_t,8> gpr;std::array<uint8_t,96> xmm;};
extern "C" {
void CvrRenderGraphCacheDetour();
void NativeCacheProbe();
void CacheProbe(void(*)());
void* CvrRenderGraphCacheOriginal=reinterpret_cast<void*>(&NativeCacheProbe);
Registers CacheRegisters{};
uint64_t CacheFifth{},CacheExpectedReturn{},CacheReturn{};
uint32_t CacheError{},CacheModify{};
}
int main() {
    CacheProbe(&NativeCacheProbe);const auto original=CacheRegisters;const auto fifth=CacheFifth;
    CacheProbe(&CvrRenderGraphCacheDetour);
    if(CacheError || CacheFifth!=fifth || CacheReturn!=CacheExpectedReturn || std::memcmp(&original,&CacheRegisters,sizeof(original)))return 1;
    CacheModify=1;CacheProbe(&CvrRenderGraphCacheDetour);
    auto expected=original;expected.gpr[3]^=0x55;
    if(CacheError || CacheFifth!=fifth || CacheReturn!=CacheExpectedReturn || std::memcmp(&expected,&CacheRegisters,sizeof(expected)))return 2;
    std::cout<<"PASS five native arguments, return address, volatile registers, XMM0-5, flags; only R8 changes\n";
}
