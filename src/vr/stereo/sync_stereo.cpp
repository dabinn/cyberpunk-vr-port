// Path B (Sync Sequential)  M-B1 observational CALLER1 hook. See sync_stereo.h.

#include "sync_stereo.h"
#include "logger.h"
#include "vrcam_config.h"   // vrcam.json access + CName hashing, shared with the launcher
#include "../render/color_blit.h"   // HUD debug overlay on the mirror image

#include <windows.h>
#include <d3d12.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <dxgi1_4.h>
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "MinHook.h"
#include "../../common/log_throttle.h"

// Storage for the switch declared in logger.h. On by default: when stereo fails to come up,
// the install sequence is the first thing worth reading.
extern "C" __declspec(dllexport) int32_t CyberpunkVR_StereoLog = 1;

// The barrel dot, published by the ImGui overlay (imgui_overlay.cpp) in NDC after its own zoom
// compensation. Taking the finished number rather than re-projecting keeps the two eyes' dots
// identical by construction.
extern "C" float    CyberpunkVR_BarrelDotNdcX;
extern "C" float    CyberpunkVR_BarrelDotNdcX2;   // the second eye's own value
extern "C" float    CyberpunkVR_BarrelDotNdcY;
extern "C" float    CyberpunkVR_BarrelDotRadiusPx;
extern "C" unsigned long long CyberpunkVR_BarrelDotTick;
extern "C" int      CyberpunkVR_BarrelDotSecondEye;
extern "C" unsigned long long CyberpunkVR_DebugBarrelDotDraws;


namespace cvr {

extern "C" __declspec(dllexport) uintptr_t CyberpunkVR_DebugSecondaryManager = 0;
extern "C" __declspec(dllexport) uintptr_t CyberpunkVR_DebugSecondaryActive = 0;
extern "C" __declspec(dllexport) uintptr_t CyberpunkVR_DebugSecondaryBinding = 0;
extern "C" __declspec(dllexport) uintptr_t CyberpunkVR_DebugLastBatchManager = 0;
extern "C" __declspec(dllexport) uintptr_t CyberpunkVR_DebugLastWorkContext = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMainNodeCalls = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugSecondaryNodeCalls = 0;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugMainNodeUnique = 0;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugSecondaryNodeUnique = 0;
extern "C" __declspec(dllexport) uintptr_t CyberpunkVR_DebugMainNodeWorks[256] = {};
extern "C" __declspec(dllexport) uintptr_t CyberpunkVR_DebugSecondaryNodeWorks[256] = {};
// WORK-verification: per-node (index parallel to *NodeWorks) accumulated rdtsc
// cycles + call counts inside the work_fn, per view. A node that DISPATCHES but
// internally SKIPS (early-out) shows near-zero avg cycles for that view -> lets
// us prove WORK (not just dispatch) and compare vrcam vs main per node.
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMainNodeCycles[256] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugSecondaryNodeCycles[256] = {};
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugMainNodeCallN[256] = {};
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugSecondaryNodeCallN[256] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMirrorCopyArms = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMirrorGameCopies = 0;
extern "C" __declspec(dllexport) uintptr_t CyberpunkVR_DebugEyeItem = 0;
extern "C" __declspec(dllexport) uintptr_t CyberpunkVR_DebugEyeBuildMgr = 0;
extern "C" __declspec(dllexport) uint32_t  CyberpunkVR_DebugEyeBuildMode = 0xFFFFFFFF;
extern "C" __declspec(dllexport) uint64_t  CyberpunkVR_DebugEyeBuildHits = 0;
extern "C" __declspec(dllexport) uintptr_t CyberpunkVR_DebugEyeFa0 = 0;
extern "C" __declspec(dllexport) uint64_t  CyberpunkVR_DebugMainFgHits = 0;
extern "C" __declspec(dllexport) uint32_t  CyberpunkVR_DebugMainBuildMode = 0xFFFFFFFF;
// eye-X manager state (view/context count @+0x54, view array @+0x48, queued
// request slot @+296)  determines whether we can queue a context request.
extern "C" __declspec(dllexport) uint32_t  CyberpunkVR_DebugEyeMgrCount = 0xFFFFFFFF;
extern "C" __declspec(dllexport) uintptr_t CyberpunkVR_DebugEyeMgrArray = 0;
extern "C" __declspec(dllexport) uintptr_t CyberpunkVR_DebugEyeMgrQueued = 0;
extern "C" __declspec(dllexport) uint32_t  CyberpunkVR_DebugMainMgrCount = 0xFFFFFFFF;
// main view context-create identity (captured by the ctx-capture probe below).
extern "C" __declspec(dllexport) void*    CyberpunkVR_DebugMainManager = nullptr;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMainCtxKey = 0;
extern "C" __declspec(dllexport) int      CyberpunkVR_DebugMainCtxA5 = -1;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCtxCreateCalls = 0;

// Descriptor-heap probe/enlarge (Path A: full second eye needs a bigger
// shader-visible CBV_SRV_UAV heap than the engine's 1,000,000 request).
extern "C" __declspec(dllexport) uint64_t  CyberpunkVR_DebugDescHeapCreates = 0;
extern "C" __declspec(dllexport) uint32_t  CyberpunkVR_DebugDescHeapSVNum = 0;    // last shader-visible CBV_SRV_UAV NumDescriptors requested
extern "C" __declspec(dllexport) uintptr_t CyberpunkVR_DebugDescHeapSVRetRva = 0; // caller RVA that requested it
extern "C" __declspec(dllexport) uintptr_t CyberpunkVR_DebugDescHeapSVRetAbs = 0; // absolute caller return address
extern "C" __declspec(dllexport) uint32_t  CyberpunkVR_DebugDescHeapSVFlags = 0;
extern "C" __declspec(dllexport) uint64_t  CyberpunkVR_DebugDescHeapEnlarged = 0;

namespace {

// --- RE constants (image base 0x140000000, verified vs build 2.31) ----------
constexpr uintptr_t CALLER1_RVA = 0x292A54;  // sub_140292A54 RenderFull
constexpr uintptr_t LIGHT_RVA   = 0x29A5B0;  // sub_14029A5B0 RenderLight (M-B2)
constexpr uintptr_t FLUSH_RENDER_SCENE_RVA = 0x293AF4;
constexpr uintptr_t RELEASE_HANDLE_RVA     = 0x1E6680;
constexpr uintptr_t APPEND_TYPEA_VIEW_RVA  = 0xD6E480;
constexpr uintptr_t VIEW_CONTEXT_ALLOC_RVA = 0x810818;  // sub_140810818: build view-tail owner X (own manager)
constexpr uintptr_t VIEW_ITEM_VTABLE_RVA   = 0x2AC8688;
constexpr uintptr_t CAMERA_WRITE_RVA       = 0x788A9C;
constexpr uintptr_t FRAME_GATE_RVA         = 0x291748;
constexpr uintptr_t VIEW_FINALIZE_RVA      = 0x29C81C;
constexpr uintptr_t FG_BUILD_RVA           = 0xAA3904;
constexpr uintptr_t TYPE_B_SUBMIT_RVA      = 0x293568;
constexpr uintptr_t VIEW_ITEM_FACTORY_RVA  = 0x293C7C;
constexpr uintptr_t JOB_CONTEXT_INIT_RVA   = 0x218B34;
constexpr uintptr_t JOB_CONTEXT_LINK_RVA   = 0x142838;
constexpr uintptr_t JOB_CONTEXT_ADVANCE_RVA = 0x141B10;
constexpr uintptr_t JOB_CONTEXT_DESTROY_RVA = 0x142F88;
constexpr uintptr_t RUN_RENDER_NODES_RVA   = 0x219730;
constexpr uintptr_t RUN_RENDER_MANAGER_LOAD_RVA = 0x2197A4;
constexpr uintptr_t RUN_NODE_BATCH_SUBMIT_RVA = 0xA9BA28;
constexpr uintptr_t RUN_NODE_BATCH_WORK_RVA   = 0xAC4A04;
constexpr uintptr_t GRAPH_REQUEST_BUILD_RVA = 0x36FCD0;
constexpr uintptr_t GRAPH_CONTEXT_PREPARE_RVA = 0x79ACA0;
constexpr uintptr_t PREPARE_COLLECTOR_WORK_RVA = 0x79B03C;
constexpr uintptr_t CAMERA_RESOURCE_SCOPE_WORK_RVA = 0xC992DC;
constexpr uintptr_t FRAME_BUILD_MARKER_RVA = 0x244AE0;
constexpr uintptr_t SYNCHRONIZE_NODE_WORK_RVA = 0x58DA9C;
constexpr uintptr_t PREPARE_SCENE_NODE_WORK_RVA = 0x784ABC;
constexpr uintptr_t GRAPH_CONTEXT_RESET_RVA = 0x79C05C;
constexpr uintptr_t GRAPH_CONTEXT_OWNER_MOVE_RVA = 0x79CDB8;
constexpr uintptr_t NODE_DISPATCH_RVA = 0x1EC404;
constexpr uintptr_t COPY_TO_TEXTURE_WORK_RVA = 0x377B58;
// The TRUE vrcam final color is written by RenderFinal2D (sub_140209FF0) into
// Resource_96070 (2444x2444), which then rests in NON_PIXEL_SHADER_RESOURCE (64).
// CopyToTexture runs earlier (build order) -> its target is the pre-final (black),
// which is why capturing it gave black regardless of copy state.
constexpr uintptr_t RENDER_FINAL2D_WORK_RVA = 0x209FF0;
// Final-color producer work-fns (main-only by cull; enable for VRCAM via node-needed).
constexpr uintptr_t DECLARE_FINAL_ONLY_WORK_RVA   = 0x1EE4A0; // DeclareCommonResourceAllocs_FinalOnly
constexpr uintptr_t EXTRACTION_FINAL_COLOR_WORK_RVA = 0x209CD4; // ExtractionFinalColor
constexpr uintptr_t CLEAR_FINAL_COLOR_WORK_RVA    = 0x209DA0; // ClearFinalColorTarget
constexpr uintptr_t GRAPH_REQUEST_REGISTER_RVA = 0x2906A28;
constexpr uintptr_t GRAPH_REQUEST_ALLOC_RVA = 0x290654C;
constexpr uintptr_t GRAPH_REQUEST_POPULATE_RVA = 0x29067B0;
constexpr uintptr_t GRAPH_MANAGER_ALLOC_RVA = 0x2926550;
constexpr uintptr_t GRAPH_MANAGER_CTOR_RVA = 0x87B164;
std::atomic<bool> g_mirror_copy_armed{false};
std::atomic<uint32_t> g_mirror_src_state{ (uint32_t)D3D12_RESOURCE_STATE_COMMON };
// VRCAM own committed render target: the engine renders the vrcam final DIRECTLY into a
// stable resource WE own (redirected at the ctx-keyed RenderFinal2D node -> view-aware,
// NOT resolution-based, scales to identical VR eyes). Committed => never aliased with
// main's transients => always holds the correct vrcam final (kills the bright/dark race).
static ID3D12Resource*             g_own_target = nullptr;
static ID3D12DescriptorHeap*       g_own_rtv_heap = nullptr;
static D3D12_CPU_DESCRIPTOR_HANDLE g_own_rtv{0};
static std::mutex                  g_own_target_mtx;
static ID3D12Resource*       g_d12_mtex = nullptr;
// mtex was created with ALLOW_RENDER_TARGET, so the HUD debug overlay may be drawn onto it.
static bool                  g_d12_mtex_is_rt = false;
// Fullscreen premultiplied-alpha pass used only for that overlay. Lives here rather than in the
// capture path because it runs on OUR mirror list, which exists whenever the mirror window does.
static ColorBlit             g_hud_mirror_blit;
static ID3D12Fence*          g_d12_fence = nullptr;
static std::atomic<uint64_t> g_d12_fence_next{1};
static std::atomic<uint64_t> g_d12_ready{0};
static std::atomic<bool>     g_d12_present_started{false};
static std::mutex            g_d12_mtx;
static UINT                  g_d12_w = 0, g_d12_h = 0;
static DXGI_FORMAT           g_d12_fmt = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugMirrorSrcState = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMirrorBarrierHits = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMirrorPendingHits = 0;
std::atomic<uint64_t> g_mirror_vrcam_serial{0};
std::atomic<uint64_t> g_mirror_armed_serial{0};

// ---- VRCAM identity: selected at runtime, never hardcoded -------------------
// The whole stereo path recognises the VRCAM view by ONE value: the CName hash of the RTT
// component's virtualCameraName, stored by the engine at view-ctx+0x28. There is now one
// authored entRenderToTextureCameraComponent per render resolution
// (vrcam_<W>x<H> / virtualCameraName vrcam_feed_<W>x<H>, all isEnabled=0), so that hash is
// DIFFERENT for every resolution and a literal would only ever match one of them. It is
// therefore derived from the selection file at init. Nothing here looks at aspect ratio or at
// resolution numbers to find the view -- only at the configured name.
// Selection file (written by the launcher, also read by modules/vrcam_select.lua which flips
// isEnabled on the matching component through the game's RTTI):
//   <game>\bin\x64\plugins\cyber_engine_tweaks\mods\CyberpunkVRPort_Stereo\vrcam.json
// with a fallback next to this DLL. CET sandboxes Lua file IO to the mod folder, which is why
// the canonical copy lives there and this side reaches out to it instead of the reverse.
// File access + CName hashing live in vrcam_config.h, shared with the launcher dialog.
// Defaults are the LEGACY single-component names, so deleting vrcam.json returns the mod to
// exactly the pre-per-resolution behaviour. That is the escape hatch if the expanded entity has
// not been imported yet: a stale selection would otherwise mean "no view recognised at all".
static char     g_vrcam_component[96] = "vrcam";
static char     g_vrcam_camera[96]    = "vrcam_feed";
// Atomic: the watcher thread below can replace it once CET reports the component's real
// virtualCameraName, while the render hooks compare against it every dispatch.
static std::atomic<uint64_t> g_vrcam_ctx_key{0x8D23967F656EA945ULL};  // cname_hash("vrcam_feed")

// ---- MAIN gameplay view identity --------------------------------------------------------
// Aspect ratio cannot answer this: in VR MAIN renders square, exactly like VRCAM, so the old
// `key == 0 && aspect > 1.3f` test either misses MAIN or latches some other wide helper view.
// The engine does mark MAIN structurally -- Present and StartRender are MAIN-only nodes
// (vrcam calls/frame = 0.00 in docs/vrcam_node_audit_v2.md) -- but those nodes carry NO view
// ctx at work_context+0x18; they reach the view OBJECT through the work-context vtable
// (engine_re/dumps/nodes/work_000.md). Hence two steps, both cheap pointer compares after the
// first frame:
//   1. the node dispatcher records the view OBJECT when it sees a MAIN-only node;
//   2. the camera writer -- the one place that sees OBJECT and CTX together for every view --
//      pins the matching ctx.
constexpr uint32_t MAIN_PRESENT_WORK_RVA     = 0x21CDC8;   // CRenderNode_Present
constexpr uint32_t MAIN_STARTRENDER_WORK_RVA = 0x21AB08;   // CRenderNode_StartRender
static std::atomic<uintptr_t> g_main_view_obj{0};
static std::atomic<uintptr_t> g_main_view_ctx{0};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMainObjBinds = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMainCtxBinds = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMainCtx = 0;
static uintptr_t sl_view_obj(void* work_context);   // defined with the camera writer below

// True for MAIN's view ctx. The aspect test survives only as a bootstrap for the first frames,
// before the cache is warm; once it is, it is never consulted again -- which is what keeps
// this correct when MAIN goes square.
static inline bool is_main_view(const void* view) {
    if (!view) return false;
    const auto* p = reinterpret_cast<const uint8_t*>(view);
    // Belt and braces: whatever the cache says, a view carrying the VRCAM camera name is never
    // MAIN. Without this, one bad pin silently disables every VRCAM branch that sits in an
    // `else if` after an is_main_view() test -- which is exactly how the fov write came out as
    // a no-op the first time round.
    if (*reinterpret_cast<const uint64_t*>(p + 0x28) != 0) return false;
    const uintptr_t cached = g_main_view_ctx.load(std::memory_order_acquire);
    if (cached) return reinterpret_cast<uintptr_t>(view) == cached;
    return *reinterpret_cast<const float*>(p + 0x98) > 1.3f;
}
// Render resolution of the selected component, parsed from its <W>x<H> name suffix. The
// mirror's RTV filter needs it to recognise the VRCAM target among every render target the
// game creates, and it must NOT be a literal вЂ” it changes with the pick.
static std::atomic<uint32_t> g_vrcam_sel_w{0};
static std::atomic<uint32_t> g_vrcam_sel_h{0};
// The resolution the launcher pick resolves to, parsed from the component name. The
// resolution override uses this as its fallback source, so MAIN still follows the pick
// even when vrport-launcher.ini was not written (dialog dismissed, file not writable).
extern "C" __declspec(dllexport) int CyberpunkVR_GetSelectedResolution(uint32_t* w, uint32_t* h) {
    const uint32_t sw = g_vrcam_sel_w.load(std::memory_order_relaxed);
    const uint32_t sh = g_vrcam_sel_h.load(std::memory_order_relaxed);
    if (!sw || !sh) return 0;
    if (w) *w = sw;
    if (h) *h = sh;
    return 1;
}
extern "C" __declspec(dllexport) const char* CyberpunkVR_VrcamComponentName() { return g_vrcam_component; }
// CName of the VRCAM camera COMPONENT (the "vrcam_<W>x<H>" object), not of its feed.
//
// This is how MAIN and VRCAM are told apart at the camera writer. The camera object is an
// Entity/IPlacedComponent (its vtable slot 68 returns that string) and carries its own
// component name at obj+0x40: measured live, the player's camera reads 0x6FCFDF926F11594E,
// which is exactly cname_hash("camera"). So the field is a per-instance identity that
// costs one load -- no view plumbing, no first/last/most-frequent guessing, and it keeps
// working across launches because it is a name hash, not an address.
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_VrcamCamNameHash() {
    return cname_hash(g_vrcam_component);
}
extern "C" __declspec(dllexport) const char* CyberpunkVR_VrcamCameraName()    { return g_vrcam_camera; }
extern "C" __declspec(dllexport) uint64_t    CyberpunkVR_VrcamCtxKey()        { return g_vrcam_ctx_key.load(); }
// 1 when the key came from the file, 0 when the built-in default is in use.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugVrcamConfigLoaded = 0;

// Enable/disable the VRCAM component at runtime. The component's isEnabled lives behind the
// game's RTTI, which only Lua can reach, so this writes a request into the CET mod's bridge
// folder and modules/vrcam_select.lua acts on it. 1 = the selected component on (default),
// 0 = every vrcam component off, i.e. the engine stops rendering the second view entirely.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_VrcamEnabled = 1;
// Mirror/stereo chain diagnostics, always on (the comparisons they count already happen):
//   NodeHits    - node dispatches for a view matching our key   (0 => wrong key / not enabled)
//   CopyNodeHits- that view's RenderFinal2D node                (0 => VRCAM has no final blit)
//   RtvHits     - its render target captured                    (0 => OMSetRenderTargets missed)
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugVrcamNodeHits = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMirrorCopyNodeHits = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMirrorRtvHits = 0;
extern "C" __declspec(dllexport) void CyberpunkVR_SetVrcamEnabled(uint32_t on) {
    CyberpunkVR_VrcamEnabled = on ? 1u : 0u;
    if (vrcam_bridge_write("vrcam_enable.txt", on ? "1" : "0"))
        cvr::log("[vrcam] request %s -> bridge/vrcam_enable.txt", on ? "ENABLE" : "DISABLE");
    else
        cvr::log("[vrcam] FAILED to write bridge/vrcam_enable.txt (request %s ignored)",
                on ? "ENABLE" : "DISABLE");
}

// ---- ONE place that changes the VRCAM selection ------------------------------------------
//
// Four things describe this one choice and they have to move together: the component name,
// the camera name, the view key hashed from the camera name, and the resolution the RTV filter
// matches on. Updating a subset is not a small bug -- a key that names no live view means no
// second eye and no mirror window, while every log line still prints the component you
// expected. That is exactly how the last one hid.
static bool g_vrcam_pick_authoritative = false;   // the launcher resolution decided it

// THE LAUNCHER PICK ARRIVES AFTER WE HAVE ALREADY READ IT.
//
// vrport-launcher.ini is written by the dxgi proxy's dialog, and that dialog runs when the
// swapchain is created. This plugin is a RED4ext plugin: it initialises long before that. So the
// read below, done once at init, returns the resolution of the PREVIOUS session, and the VRCAM
// component is picked one launch behind -- pick 3072 and the log says
//     [vrcam] launcher picked 2560x2560 -> switching component ...
// while a hundred lines later the same log says
//     CreateSwapChainForHwnd override: 1024x768 -> 3072x3072
// The file was never wrong; we simply looked at it too early.
//
// So the read is a function now, and the watcher below repeats it every poll. Whatever the
// resolution turns out to be once the swapchain exists, the component follows it.
static bool vrcam_launcher_resolution(int* w, int* h) {
    char iniPath[MAX_PATH] = {};
    if (!path_beside_module(nullptr, "vrport-launcher.ini", iniPath, sizeof(iniPath)))
        return false;
    FILE* f = fopen(iniPath, "rb");
    if (!f) return false;
    char buf[512] = {};
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    int lw = 0, lh = 0;
    if (const char* p = strstr(buf, "width=")) lw = atoi(p + 6);
    if (const char* p = strstr(buf, "height=")) lh = atoi(p + 7);
    if (lw <= 0 || lh <= 0) return false;
    if (w) *w = lw;
    if (h) *h = lh;
    return true;
}


static void vrcam_apply_selection(const char* component, const char* camera) {
    strncpy_s(g_vrcam_component, sizeof(g_vrcam_component), component, _TRUNCATE);
    strncpy_s(g_vrcam_camera, sizeof(g_vrcam_camera), camera, _TRUNCATE);
    g_vrcam_ctx_key.store(cname_hash(g_vrcam_camera));
    int w = 0, h = 0;
    // Resolution off the COMPONENT name, falling back to the camera's -- both carry <W>x<H>.
    if (vrcam_parse_resolution(g_vrcam_component, &w, &h) ||
        vrcam_parse_resolution(g_vrcam_camera, &w, &h)) {
        g_vrcam_sel_w.store((uint32_t)w);
        g_vrcam_sel_h.store((uint32_t)h);
    }
    cvr::log("[vrcam] selection applied: component=%s camera=%s key=0x%016llX res=%dx%d%s",
            g_vrcam_component, g_vrcam_camera,
            (unsigned long long)g_vrcam_ctx_key.load(), w, h,
            g_vrcam_pick_authoritative ? " (from launcher resolution)" : "");
}

// Read the selection file and derive the view key from it. Called once from
// sync_stereo_init(), i.e. before any hook can observe a view.
static void load_vrcam_selection() {
    std::string text;
    char path[MAX_PATH] = {};
    if (!vrcam_config_read(&text, path, sizeof(path))) {
        cvr::log("[vrcam] no vrcam.json (looked at %s) -> keeping default component=%s camera=%s "
                "key=0x%016llX", path, g_vrcam_component, g_vrcam_camera,
                (unsigned long long)g_vrcam_ctx_key);
        return;
    }
    std::string comp;
    if (!json_find_string(text, "component", &comp)) {
        cvr::log("[vrcam] %s has no \"component\" field -> keeping default %s",
                path, g_vrcam_component);
        return;
    }

    // THE LAUNCHER PICK WINS over a stale "component" field.
    //
    // Two files describe one decision: vrport-launcher.ini carries the resolution the user
    // chose, vrcam.json carries which VRCAM component is active. When they disagree the
    // symptom is silent and total -- the view key is hashed from the CAMERA name, so a
    // 2444 key against a running 2560 view matches nothing: no second eye, no mirror window,
    // and every log line still cheerfully naming the component from the file. Exactly the
    // "component is definitely on but there is no second eye" case.
    //
    // So: if the launcher's resolution names a component the entity actually has (it must be
    // in the authored "components" list -- selecting one that was never imported would render
    // nothing at all), that one wins, and it is written back so the CET side, which reads the
    // same file to decide what to enable, agrees with us.
    {
        int lw = 0, lh = 0;
        vrcam_launcher_resolution(&lw, &lh);
        if (lw > 0 && lh > 0) {
            char wanted[128] = {};
            _snprintf_s(wanted, sizeof(wanted), _TRUNCATE, "vrcam_%dx%d", lw, lh);
            if (comp != wanted) {
                std::vector<std::string> authored;
                json_find_string_array(text, "components", &authored);
                bool exists = false;
                for (const std::string& a : authored) if (a == wanted) { exists = true; break; }
                if (exists) {
                    cvr::log("[vrcam] launcher picked %dx%d -> switching component %s -> %s",
                            lw, lh, comp.c_str(), wanted);
                    comp = wanted;
                    // Authoritative from here on: the watcher must not adopt a stale live
                    // camera over this, or the selection oscillates and never settles.
                    g_vrcam_pick_authoritative = true;
                    if (vrcam_config_write_component(wanted))
                        cvr::log("[vrcam] vrcam.json updated so the CET side enables the same one");
                    else
                        cvr::log("[vrcam] WARNING: could not write vrcam.json -- the CET side may "
                                "still enable something else, leaving no matching view");
                } else {
                    cvr::log("[vrcam] launcher picked %dx%d but %s is not in the authored list "
                            "-> keeping %s (import the component, then add it to \"components\")",
                            lw, lh, wanted, comp.c_str());
                }
            }
        }
    }
    // The camera name is what the view key is hashed from, so it MUST match the component.
    // An explicit "virtualCamera" is only honoured when it agrees with the component; a stale
    // one (e.g. the component was changed and that field was not) is ignored with a warning
    // instead of silently producing a key that matches no view -- the failure mode there is
    // "no stereo and no mirror window", with every log line still naming the right component.
    char derived[128] = {};
    const bool can_derive = vrcam_derive_camera(comp.c_str(), derived, sizeof(derived));
    std::string cam;
    const bool explicit_cam = json_find_string(text, "virtualCamera", &cam);
    if (explicit_cam && can_derive && cam != derived) {
        cvr::log("[vrcam] WARNING: \"virtualCamera\"=%s does not match component %s "
                "(expected %s) -> using %s", cam.c_str(), comp.c_str(), derived, derived);
        cam = derived;
    } else if (!explicit_cam) {
        if (!can_derive) {
            cvr::log("[vrcam] component %s has no \"vrcam_\" prefix and no \"virtualCamera\" "
                    "given -> cannot derive the camera name, keeping default %s",
                    comp.c_str(), g_vrcam_camera);
            return;
        }
        cam = derived;
    }
    vrcam_apply_selection(comp.c_str(), cam.c_str());
    CyberpunkVR_DebugVrcamConfigLoaded = 1;
    cvr::log("[vrcam] selection source: %s", path);
}

// The camera name above is DERIVED from the component name, which assumes the asset follows
// the vrcam_feed_<suffix> convention. When it does not вЂ” a component renamed in WolvenKit whose
// virtualCameraName kept the old value вЂ” the key matches no view and the entire VR path goes
// quiet (no stereo, no mirror) while every log line still shows the right component.
// So the authored name is treated as a HINT and the ground truth comes from the live entity:
// modules/vrcam_select.lua reads virtualCameraName off the component it actually enabled and
// writes it to bridge/vrcam_active.txt; this watcher adopts it.
static void vrcam_active_watcher() {
    char path[MAX_PATH] = {};
    if (!vrcam_bridge_path("vrcam_active.txt", path, sizeof(path))) return;
    std::string last;
    uint64_t disagreeSince = 0;      // first tick of a disagreement that produced no nodes
    uint64_t lastRequest = 0;        // rate-limit the re-request writes
    uint64_t lastNodeHits = 0;
    int seenW = 0, seenH = 0;         // last launcher resolution acted on
    for (;;) {
        Sleep(500);

        // RE-READ THE LAUNCHER PICK. See vrcam_launcher_resolution: the init-time read happens
        // before the proxy's dialog has written the file, so it always returns the previous
        // session's resolution. Here the swapchain exists and the file is current.
        {
            int lw = 0, lh = 0;
            if (vrcam_launcher_resolution(&lw, &lh) && (lw != seenW || lh != seenH)) {
                seenW = lw; seenH = lh;
                char wanted[128] = {};
                _snprintf_s(wanted, sizeof(wanted), _TRUNCATE, "vrcam_%dx%d", lw, lh);
                if (strcmp(wanted, g_vrcam_component) != 0) {
                    std::string text;
                    char cfg[MAX_PATH] = {};
                    std::vector<std::string> authored;
                    bool exists = false;
                    if (vrcam_config_read(&text, cfg, sizeof(cfg)) &&
                        json_find_string_array(text, "components", &authored)) {
                        for (const std::string& a : authored)
                            if (a == wanted) { exists = true; break; }
                    }
                    char camera[128] = {};
                    if (exists && vrcam_derive_camera(wanted, camera, sizeof(camera))) {
                        cvr::log("[vrcam] launcher resolution is %dx%d (read after the swapchain "
                                 "existed) -> switching %s -> %s", lw, lh, g_vrcam_component, wanted);
                        vrcam_apply_selection(wanted, camera);
                        g_vrcam_pick_authoritative = true;
                        vrcam_config_write_component(wanted);
                        vrcam_bridge_write("vrcam_enable.txt", "1");
                        last.clear();
                        disagreeSince = 0;
                        continue;
                    }
                    if (!exists)
                        cvr::log("[vrcam] launcher resolution is %dx%d but %s is not in the "
                                 "authored \"components\" list -> keeping %s",
                                 lw, lh, wanted, g_vrcam_component);
                }
            }
        }
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        char buf[128] = {};
        const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        for (char* p = buf; *p; ++p)                      // trim trailing whitespace/newlines
            if (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') { *p = '\0'; break; }
        if (buf[0] == '\0') continue;                     // empty = VRCAM disabled, keep the key
        if (last == buf) continue;
        last = buf;
        const uint64_t key = cname_hash(buf);
        if (key == g_vrcam_ctx_key.load()) {
            cvr::log("[vrcam] live camera confirms key: %s", buf);
            continue;
        }
        if (g_vrcam_pick_authoritative) {
            // The launcher resolution decided this, so CET is the one out of step -- do NOT
            // adopt. Adopting is what made the selection oscillate between the two resolutions
            // on every poll, and the view key with it, so nothing downstream ever settled.
            // Re-state the request and give CET a few seconds to catch up.
            // Give up on EVIDENCE, never on a stopwatch alone.
            //
            // VRCAM lives on the player entity, so it only exists in gameplay: a plain attempt
            // counter burns through its budget in the main menu -- against a stale name left in
            // vrcam_active.txt by the previous session -- and abandons a perfectly good pick
            // before the game has even loaded. So the clock only counts while our own view is
            // producing NOTHING (node hits frozen at zero) and something else is being reported
            // live. In the menu nothing renders either way, and the moment gameplay starts with
            // the right component the hits move and this resets.
            const uint64_t nodeHits = CyberpunkVR_DebugVrcamNodeHits;
            if (nodeHits != lastNodeHits) { lastNodeHits = nodeHits; disagreeSince = 0; }
            if (nodeHits != 0) { last.clear(); continue; }   // our view IS rendering -- fine
            const uint64_t nowTick = GetTickCount64();
            if (!disagreeSince) disagreeSince = nowTick;
            if (nowTick - disagreeSince < 30000) {
                if (nowTick - lastRequest >= 2000) {         // re-state, but not every poll
                    lastRequest = nowTick;
                    cvr::log("[vrcam] live camera is %s but the launcher picked %s -> NOT "
                            "adopting; re-requesting %s", buf, g_vrcam_camera, g_vrcam_component);
                    vrcam_config_write_component(g_vrcam_component);
                    vrcam_bridge_write("vrcam_enable.txt", "1");
                }
                last.clear();               // re-evaluate on the next poll
                continue;
            }
            // It never switched. Almost always this means the component the launcher's
            // resolution names is not actually ON THE PLAYER ENTITY -- listing it in
            // vrcam.json does not create it; it has to be imported. Say so once, then take
            // the live camera so there IS a second eye, rather than holding out for a
            // component that is never coming and showing nothing at all.
            cvr::log("[vrcam] GIVING UP on %s -- 30 s of gameplay with another camera live and "
                    "zero nodes of our own, so the entity most likely has no such component "
                    "(listing it in vrcam.json does not create it; the asset has to be "
                    "imported). Falling back to the live camera %s.",
                    g_vrcam_component, buf);
            g_vrcam_pick_authoritative = false;
        }
        disagreeSince = 0;
        cvr::log("[vrcam] live camera is %s (key 0x%016llX), not %s -> adopting the live one "
                "(no launcher pick to honour); check virtualCameraName on component %s",
                buf, (unsigned long long)key, g_vrcam_camera, g_vrcam_component);
        vrcam_apply_selection(g_vrcam_component, buf);
    }
}

// ---- CPU profiling (phase 0): where does the 2-view CPU cost go? ------------
// QPC wall-clock accumulators, split main vs vrcam, averaged over a window and
// published to the overlay. Goal: locate the CPU bottleneck (graph build vs node
// dispatch vs submit) before optimizing the simultaneous true-stereo path.
static LARGE_INTEGER g_qpc_freq = { };
static double        g_qpc_to_ms = 0.0;   // 1000/freq, set in init
static inline int64_t prof_now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
// per-window raw accumulators (reset each publish window)
static std::atomic<int64_t>  g_prof_build_main_ticks{0};   // FullBuild+IncrBuild, main views
static std::atomic<int64_t>  g_prof_build_vrcam_ticks{0};  // FullBuild+IncrBuild, vrcam view
static std::atomic<uint64_t> g_prof_build_main_calls{0};
static std::atomic<uint64_t> g_prof_build_vrcam_calls{0};
static std::atomic<int64_t>  g_prof_disp_main_ticks{0};    // NodeDispatch orig call, main-thread nodes
static std::atomic<int64_t>  g_prof_disp_vrcam_ticks{0};   // NodeDispatch orig call, vrcam nodes
static std::atomic<uint64_t> g_prof_disp_main_nodes{0};
static std::atomic<uint64_t> g_prof_disp_vrcam_nodes{0};
static std::atomic<int64_t>  g_prof_frame_last{0};         // last Present QPC (frame wall time)
static std::atomic<uint64_t> g_prof_frames{0};             // frames since last audit dump/reset
// Audit denominators. Present count is NOT a safe divisor (frame generation presents more
// often than the engine builds views), so the dump also carries the number of TOP-LEVEL
// node dispatches per view -- that is exactly one per rendered view-frame -- plus the wall
// window. The DLL therefore emits raw totals and lets the parser normalise.
static std::atomic<uint64_t> g_prof_top_main{0};
static std::atomic<uint64_t> g_prof_top_vrcam{0};
static std::atomic<int64_t>  g_prof_window_t0{0};          // QPC at window start
// published averages (ms) read by the overlay
extern "C" __declspec(dllexport) double   CyberpunkVR_ProfBuildMainMs = 0.0;
extern "C" __declspec(dllexport) double   CyberpunkVR_ProfBuildVrcamMs = 0.0;
extern "C" __declspec(dllexport) double   CyberpunkVR_ProfDispMainMs = 0.0;
extern "C" __declspec(dllexport) double   CyberpunkVR_ProfDispVrcamMs = 0.0;
extern "C" __declspec(dllexport) double   CyberpunkVR_ProfFrameMs = 0.0;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_ProfDispVrcamNodes = 0;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_ProfDispMainNodes = 0;
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_ProfEnable = 0;   // master toggle (OFF: profiler dormant, no per-node QPC/atomic cost)
// Called once per frame from the overlay's Present hook (via the exported symbol
// below); averages the window and resets. window = frames since last publish.
extern "C" __declspec(dllexport) void CyberpunkVR_ProfPublish() {
    if (!g_qpc_to_ms) return;
    const int64_t now = prof_now();
    const int64_t prev = g_prof_frame_last.exchange(now, std::memory_order_relaxed);
    if (prev) CyberpunkVR_ProfFrameMs = (double)(now - prev) * g_qpc_to_ms;
    g_prof_frames.fetch_add(1, std::memory_order_relaxed);
    int64_t unset = 0;                  // start the audit window at the first published frame
    g_prof_window_t0.compare_exchange_strong(unset, now, std::memory_order_relaxed);
    // per-frame instantaneous build/dispatch cost = window total (these hooks fire
    // per-frame, so the window is exactly one frame when published every frame).
    const int64_t bm = g_prof_build_main_ticks.exchange(0, std::memory_order_relaxed);
    const int64_t bv = g_prof_build_vrcam_ticks.exchange(0, std::memory_order_relaxed);
    const int64_t dm = g_prof_disp_main_ticks.exchange(0, std::memory_order_relaxed);
    const int64_t dv = g_prof_disp_vrcam_ticks.exchange(0, std::memory_order_relaxed);
    CyberpunkVR_ProfBuildMainMs  = (double)bm * g_qpc_to_ms;
    CyberpunkVR_ProfBuildVrcamMs = (double)bv * g_qpc_to_ms;
    CyberpunkVR_ProfDispMainMs   = (double)dm * g_qpc_to_ms;
    CyberpunkVR_ProfDispVrcamMs  = (double)dv * g_qpc_to_ms;
    CyberpunkVR_ProfDispMainNodes  = (uint32_t)g_prof_disp_main_nodes.exchange(0, std::memory_order_relaxed);
    CyberpunkVR_ProfDispVrcamNodes = (uint32_t)g_prof_disp_vrcam_nodes.exchange(0, std::memory_order_relaxed);
    g_prof_build_main_calls.exchange(0, std::memory_order_relaxed);
    g_prof_build_vrcam_calls.exchange(0, std::memory_order_relaxed);
}
thread_local bool t_mirror_copy_node_active = false;
thread_local ID3D12Resource* t_mirror_copy_rtv = nullptr;
thread_local DXGI_FORMAT t_mirror_copy_rtv_format = DXGI_FORMAT_UNKNOWN;
// Command list the vrcam RenderFinal2D node records into (stashed at RTV capture) and
// the engine's LAST transition StateAfter for the captured resource within the node
// (activation pattern is ALIASING + transition->RENDER_TARGET + Discard, so RT unless
// the node retransitions it later; tracked in hk_ResourceBarrier on the same thread).
thread_local ID3D12GraphicsCommandList* t_mirror_copy_list = nullptr;
thread_local uint32_t t_mirror_src_state =
    (uint32_t)D3D12_RESOURCE_STATE_RENDER_TARGET;
// Work fn of the node currently recording on this thread (set at dispatch; nested
// dispatches clobber it -- acceptable for attribution probes).
thread_local uintptr_t t_current_node_work = 0;
// True while ANY vrcam-ctx node records on this thread (view attribution for hooks).
thread_local bool t_vrcam_node_active = false;

// View attribution for the ENGINE camera hooks (LocateCamera / PatchCamera /
// FinalCamera / NormalFov / Unifix / ProjStage in vr_core.cpp).
//
// Those hooks are mid-function byte patches with no notion of a view -- they were
// written when MAIN was the only one. IDA puts them inside per-view render-graph work:
// FinalCamera sits in sub_1407854C0, whose only caller is sub_140784ABC (PrepareScene
// node work); NormalFov is in the rect-compute node; Unifix in graph-request-build.
// All three run once PER VIEW, so enabling the VRCAM component makes each fire twice a
// frame -- once for a camera that is not MAIN -- and their shared per-frame state
// (g_lastLocateSeq, g_lastLocateQuat, g_renderedSeq) ends up describing whichever view
// ran last. MAIN is then submitted with a pose belonging to the other camera, which is
// the judder that appears only with the component enabled.
//
// The dispatcher already tags the executing view, so the hooks can simply ask.
extern "C" __declspec(dllexport) int CyberpunkVR_IsVrcamViewActive() {
    return t_vrcam_node_active ? 1 : 0;
}

// EXACT view identity, because "not VRCAM" is not the same as "is MAIN".
//
// The dispatcher already reads the view key at ctx+0x28. MAIN is key 0 -- that is how
// Detour_FlagCompute binds g_main_ctx -- and VRCAM is g_vrcam_ctx_key. Every OTHER view
// the engine runs (distant geometry, shadow and reflection views) carries its own key and
// sails straight through a vrcam-only gate.
//
// That matters for the camera hooks: forcing the head camera onto one of those views
// leaves its content composited against a camera it was not rendered with, so it stops
// being world-locked and slides with the head instead of staying put.
thread_local uint64_t t_active_view_key   = 0;
thread_local bool     t_active_view_known = false;

extern "C" __declspec(dllexport) int CyberpunkVR_IsMainViewActive() {
    return (t_active_view_known && t_active_view_key == 0) ? 1 : 0;
}
// Returns 0 when the caller is not inside a view-carrying node dispatch (the view is then
// genuinely unknown, not "MAIN"). Callers decide what to do with that.
extern "C" __declspec(dllexport) int CyberpunkVR_GetActiveViewKey(unsigned long long* out) {
    if (!t_active_view_known) return 0;
    if (out) *out = t_active_view_key;
    return 1;
}

// ---- WHICH VIEW OWNS A CAMERA OBJECT ------------------------------------------------
// LocateCamera runs outside the render-graph dispatch (measured: its vrcam gate never
// fired once), so the thread-local view tag above is useless there. But LocateCamera is
// also the ONLY stage where head orientation can be injected without artifacts -- it runs
// before culling, before the distant/imposter placement and before the weapon viewmodel
// is put into camera space, which is why writing later slides the world and drags the gun.
//
// So the view has to be attached to the CAMERA OBJECT instead. Both cameras are instances
// of the same class (vtable rva 0x2B031D0, measured live on both), and their addresses
// change every launch, so the object alone says nothing -- but the camera writer sees the
// view ctx (key: MAIN = 0, VRCAM = g_vrcam_ctx_key) and the view's own object at the same
// moment. Finding the camera through that ctx binds object -> view exactly, with no
// first/last/most-frequent guessing.
static std::atomic<uintptr_t> g_cam_obj_main{0};
static std::atomic<uintptr_t> g_cam_obj_vrcam{0};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCamObjMain    = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCamObjVrcam   = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCamBindMain   = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCamBindVrcam  = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCamScanFails  = 0;

// 0 = this object is not (yet) bound to any view, 1 = MAIN, 2 = VRCAM.
extern "C" __declspec(dllexport) int CyberpunkVR_ClassifyCamera(const void* obj) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(obj);
    if (!v) return 0;
    if (v == g_cam_obj_main.load(std::memory_order_acquire))  return 1;
    if (v == g_cam_obj_vrcam.load(std::memory_order_acquire)) return 2;
    return 0;
}
// Dispatch nesting depth: frame-level totals are taken at depth 0 only, per-node rows at
// every depth (SceneDrv re-enters the dispatcher once per scene pass).
thread_local int t_prof_disp_depth = 0;
// Inclusive time of the child nodes dispatched by the node currently on this thread's
// stack; lets each node report self = inclusive - children. Only touched when profiling.
thread_local int64_t t_prof_child_ticks = 0;
// Tonemap OUTPUT capture (crash-safe fix): the tonemap 2-MRT pass writes RT0 = the
// tonemapped (dark/correct) post color. We snapshot RT0 into our committed g_stable_tex
// in the valid window (tonemap node epilogue) and the mirror/eye reads OUR buffer,
// bypassing Final2D's flapping version selection entirely. Pure copy of an engine-valid
// RT => no index/resolver/generation manipulation => cannot crash (unlike all the
// resolver-layer attempts). Set in the OM hook when the identified tonemap 2-MRT binds.
thread_local ID3D12Resource* t_tm_rt0 = nullptr;
thread_local ID3D12GraphicsCommandList* t_tm_rt0_list = nullptr;
// consumed flag: set false when the OM hook captures RT0; the FIRST node-dispatch
// epilogue after that consumes it once (= the tonemap node's own epilogue, its list
// still open). Avoids fragile per-dispatch reset / t_current_node_work checks that
// nested sub-node dispatches broke (DebugTonemapSnaps stayed 0).
thread_local bool t_tm_consumed = true;
// RT0's real state, tracked in hk_ResourceBarrier during the tonemap node (RT0 is NOT
// necessarily RENDER_TARGET at the node epilogue -> the wrong barrier StateBefore made
// the engine's Close/submit stall = the freeze). Init to RENDER_TARGET at capture.
thread_local uint32_t t_tm_rt0_state = (uint32_t)D3D12_RESOURCE_STATE_RENDER_TARGET;
// True once the tonemap RT0 source is available (DLSS path). When set, the Final2D
// copy is skipped (tonemap source wins); when NOT set (no DLSS), the Final2D copy runs
// as fallback -> fixes the "washed out without DLSS" regression.
static std::atomic<bool> g_have_tonemap_source{false};
extern "C" __declspec(dllexport) int32_t CyberpunkVR_StableFromTonemap = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugTonemapSnaps = 0;
// Adapted-exposure accumulators (28-byte UAV buffers, one per view; identified in the
// EXPOSURE capture: vrcam=23828, main=5712). Captured live by barrier signature:
// BUFFER W=28 transitioning UNORDERED_ACCESS -> PS|NPS right after the 1x1x1
// adaptation dispatch, attributed by t_vrcam_node_active.
static std::atomic<ID3D12Resource*> g_expo_vrcam{nullptr};
static std::atomic<ID3D12Resource*> g_expo_main{nullptr};
// The engine's per-frame constants (b0 in the composite): 30 float4, first float is the time the
// HUD flicker runs on. Captured in hk_CopyBufferRegion by its unique 480-byte upload.
static std::atomic<ID3D12Resource*> g_frame_cb{nullptr};
// The composite's own constants (b6): curvature, glow weights, aberration, halo.
static std::atomic<ID3D12Resource*> g_hud_cb{nullptr};
// True once g_hud_cb points at OUR OWN copy taken from the ring. From then on the copy-path
// capture must keep its hands off: it releases whatever it displaces, and displacing our buffer
// would free it while g_hud_cb_copy_ptr still points into it.
static std::atomic<bool> g_hud_cb_from_ring{false};

// Persistent CPU view of an engine UPLOAD heap, so a constant upload can be identified by its
// contents as it goes past. Mapping an upload resource is refcounted and read-only here; the
// mapping is deliberately never released, since the heaps live for the session.
struct MappedUpload { ID3D12Resource* res; uint8_t* ptr; uint64_t size; uint64_t va; };
// 64 SLOTS, AND REFUSALS DO NOT LIVE HERE. There used to be eight, shared between mapped rings
// and the memo of resources that turned out not to be upload heaps -- and since every
// CopyBufferRegion source is probed, the eight filled within seconds, mostly with refusals.
// After that upload_map_read returned nullptr for everything forever, so the composite's
// constants could never be found in the ring at all: a whole 17k-line session logged not one
// "found in the upload ring", and the only capture came from the weaker content test below,
// thousands of frames in. The refusal memo is a pure optimisation, so it gets its own ring that
// may be overwritten; a re-probe costs one GetHeapProperties.
static std::array<MappedUpload, 64> g_upload_maps{};
static uint32_t g_upload_map_n = 0;
static std::array<ID3D12Resource*, 64> g_upload_refused{};
static uint32_t g_upload_refused_w = 0;
static bool g_upload_map_full_logged = false;
static std::mutex g_upload_map_mtx;

// ---- finding the composite's constants where they actually live -----------------------------
//
// They are never copied anywhere: the capture shows the CBV pointing straight into the upload
// ring (Resource_41 @ 108361216), so the engine writes them there and binds them in place.
// Watching CopyBufferRegion for them was therefore looking in the wrong place entirely.
//
// The ring is CPU-visible and already mapped for us, so instead we look for them by fingerprint.
// A 256-byte-aligned block qualifies only if register 16 zw is exactly the HUD surface size (the
// composite's target) AND the rest is in range for what it claims to be -- curvature small, glow
// weights and saturation in [0,1], aberration tiny. Nothing else in the engine's constant traffic
// satisfies all of that at once.
//
// The values are graphics settings, so they change only when the user changes one: found once and
// re-checked every few seconds, at a cost of a few milliseconds on the present thread.
static ID3D12Resource* g_hud_cb_copy = nullptr;   // our own 512-byte upload CB
static uint8_t* g_hud_cb_copy_ptr = nullptr;
static uint64_t g_hud_cb_scan_tick = 0;

static bool hud_cb_block_plausible(const uint8_t* p, float w, float h) {
    __try {
        const float* r = reinterpret_cast<const float*>(p);
        if (r[16 * 4 + 2] != w || r[16 * 4 + 3] != h) return false;
        const float cxk = r[3 * 4 + 0], cyk = r[3 * 4 + 1];
        if (!(fabsf(cxk) < 0.5f && fabsf(cyk) < 0.5f)) return false;
        const float w1 = r[8 * 4 + 2], w2 = r[8 * 4 + 3], w4 = r[9 * 4 + 0];
        if (!(w1 >= 0.0f && w1 <= 1.0f && w2 >= 0.0f && w2 <= 1.0f &&
              w4 >= 0.0f && w4 <= 1.0f)) return false;
        const float ab = r[6 * 4 + 3], sat = r[6 * 4 + 2];
        if (!(ab >= 0.0f && ab < 0.01f && sat >= 0.0f && sat <= 4.0f)) return false;
        const float bg = r[5 * 4 + 0], bl = r[5 * 4 + 1];
        if (!(bg >= 0.0f && bg <= 4.0f && bl >= 0.0f && bl <= 8.0f)) return false;
        // A block of zeros passes every range test above -- and the ring is full of them. Demand
        // that the settings actually look set: a halo, a glow and a curvature all present.
        return bg > 1e-4f && (w1 > 1e-4f || w2 > 1e-4f || w4 > 1e-4f) &&
               (fabsf(cxk) > 1e-6f || fabsf(cyk) > 1e-6f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The same test, applied to an upload as it is copied rather than to a block sitting in the ring.
//
// It used to check ONLY that register 16 zw matched the target size, on the grounds that nothing
// else carries that pair -- and the log says otherwise: it captured blocks with curvature
// (0, nan), (-1.0e35, nan) and (-4.9e35, nan). A size pair is a weak fingerprint on its own
// because it is two floats that any coincidence can produce; the range checks are what make it
// unambiguous. Two capture paths for one buffer had two different ideas of "valid", and the
// permissive one always won, because it ran on every copy while the strict one only stored when
// nothing had been found yet.
static bool hud_cb_content_matches(const uint8_t* base, UINT64 off, float w, float h,
                                   float* curvature_out) {
    if (!base) return false;
    if (!hud_cb_block_plausible(base + off, w, h)) return false;
    __try {
        const float* r3 = reinterpret_cast<const float*>(base + off + 3 * 16);
        curvature_out[0] = r3[0];
        curvature_out[1] = r3[1];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static const uint8_t* upload_map_read(ID3D12Resource* res) {
    if (!res) return nullptr;
    std::lock_guard<std::mutex> lk(g_upload_map_mtx);
    for (uint32_t i = 0; i < g_upload_map_n; ++i)
        if (g_upload_maps[i].res == res) return g_upload_maps[i].ptr;
    for (ID3D12Resource* r : g_upload_refused)
        if (r == res) return nullptr;
    if (g_upload_map_n >= g_upload_maps.size()) {
        if (!g_upload_map_full_logged) {
            g_upload_map_full_logged = true;
            log("[hud] upload-map table full at %u rings -- no further heaps will be mapped",
                g_upload_map_n);
        }
        return nullptr;
    }
    auto refuse = [&]() {
        g_upload_refused[g_upload_refused_w] = res;
        g_upload_refused_w = (g_upload_refused_w + 1) % g_upload_refused.size();
    };
    D3D12_HEAP_PROPERTIES hp{};
    D3D12_HEAP_FLAGS hf{};
    if (FAILED(res->GetHeapProperties(&hp, &hf)) || hp.Type != D3D12_HEAP_TYPE_UPLOAD) {
        refuse();
        return nullptr;
    }
    void* ptr = nullptr;
    D3D12_RANGE none{0, 0};
    if (FAILED(res->Map(0, &none, &ptr)) || !ptr) {
        refuse();
        return nullptr;
    }
    res->AddRef();
    const D3D12_RESOURCE_DESC ud = res->GetDesc();
    g_upload_maps[g_upload_map_n++] = { res, static_cast<uint8_t*>(ptr), ud.Width,
                                       res->GetGPUVirtualAddress() };
    return static_cast<const uint8_t*>(ptr);
}
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugExpoVrcam = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugExpoMain = 0;
// Tonemap-INPUT probe: exposure is proven stable, so the flap is in one of the
// tonemap's texture inputs (constant transform x two static inputs = two bit-exact
// output states). Capture every >=1000px texture transitioned to PS-readable while
// the vrcam tonemap node records; sample an 8x8 center block of each per frame and
// split the means by the final image's bright/normal state -> the culprit input has
// a large split, innocent inputs ~0.
struct TmInCap {
    std::atomic<ID3D12Resource*> res{nullptr};
    std::atomic<uint32_t>        state{0};
    std::atomic<uint32_t>        fmt{0};
};
// ONE global capture set covering the whole vrcam frame: every large texture
// transitioned to a read state during ANY vrcam node, tagged with the capturing
// node's work RVA. Entries persist across frames (session resources are stable;
// dedupe keeps the set converged) -- stale entries sample stable content = innocent.
static TmInCap                  g_tm_in[24];
static std::atomic<uint32_t>    g_tm_in_n{0};
static std::atomic<uint32_t>    g_tm_in_rva[24] = {};    // capturing node work RVA
static ID3D12Resource*          g_ti_rb[4] = {};         // 64KB readback per slot
static uint8_t*                 g_ti_map[4] = {};
static uint32_t                 g_ti_count[4] = {};
static ID3D12Resource*          g_ti_src[4][24] = {};
static uint32_t                 g_ti_fmt[4][24] = {};
static uint32_t                 g_ti_tag[4][24] = {};    // node work RVA
static bool                     g_ti_valid[4] = {};
// Dedupe-append (POD-only, no locks: worst case a rare duplicate, harmless).
// Keeps the FIRST capturing node's RVA (where the texture first became readable).
static void tm_set_push(ID3D12Resource* res, uint32_t state, uint32_t fmt,
        uint32_t rva) {
    if (!res) return;
    const uint32_t n = g_tm_in_n.load(std::memory_order_acquire);
    for (uint32_t k = 0; k < n && k < 24; ++k) {
        if (g_tm_in[k].res.load(std::memory_order_relaxed) == res) {
            g_tm_in[k].state.store(state, std::memory_order_relaxed);
            return;
        }
    }
    if (n < 24) {
        res->AddRef();      // entries outlive the frame; engine may free transients
                            // on graph rebuilds (VrcamDlss toggle) -> dangling ptr ->
                            // AV in GetDesc (crash 20260720-165958, read @0x3C).
        g_tm_in[n].res.store(res, std::memory_order_relaxed);
        g_tm_in[n].state.store(state, std::memory_order_relaxed);
        g_tm_in[n].fmt.store(fmt, std::memory_order_relaxed);
        g_tm_in_rva[n].store(rva, std::memory_order_relaxed);
        g_tm_in_n.store(n + 1, std::memory_order_release);
    }
}
// Tonemap node (work RVA 0x768510) resolves its frame-graph resources with IDs
// ((work_context+0x34)<<24) XOR const -- +0x34 is the per-view graph-instance salt.
// If vrcam's tonemap salt flaps between its own and main's value, it resolves MAIN's
// adapted-exposure buffer on those frames => the two bit-exact luma states. Log salt
// TRANSITIONS for both views + export last values for x64dbg polling.
constexpr uintptr_t TONEMAP_WORK_RVA = 0x768510;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugTmSaltVrcam = 0xFFFFFFFF;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugTmSaltMain = 0xFFFFFFFF;
static std::atomic<uint32_t> g_tmsalt_last_vrcam{0xFFFFFFFF};
static std::atomic<uint32_t> g_tmsalt_last_main{0xFFFFFFFF};
// THE FLAP SELECTOR (decompiled from the RenderFinal2D node, sub_140209FF8):
//   salt = (ctx[0x30] & 1) ? 0 : ctx[0x34];   // bit0 => "borrow MAIN's instance"
//   source_id = (salt<<24) ^ 0x3D7E6258;
// With forced VrcamDlss the graph's "does this view own a post buffer" decision races
// -> bit0 flaps -> the final blit alternately reads MAIN's post buffer (bright) and
// vrcam's own (normal): two stable sources = two bit-exact luma states. The vrcam
// post chain provably runs every frame (histogram 6/frame, tonemap each frame), so
// borrowing is always wrong here: force bit0=0 for vrcam-ctx nodes.
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_ForceOwnPost = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugOwnPostFixes = 0;
static std::atomic<uint32_t> g_ctx30_last_vrcam{0xFFFFFFFF};
static std::atomic<uint32_t> g_ctx30_last_main{0xFFFFFFFF};
// Resolver probe: sub_1401F3D20 = frame-graph "declared ID -> physical index" query
// used by every node right before writing descriptors. Gated to the vrcam
// RenderFinal2D / tonemap recording threads it shows, PER FRAME, which salted ID the
// pass asked for and which physical index came back -> the selection flap becomes
// directly visible at the consumer (no more stage guessing).
constexpr uintptr_t RESOLVE_QUERY_RVA = 0x1F3D20;
using Resolve3D20Fn = __int64(__fastcall*)(__int64, uint32_t*, uint32_t*, __int64);
static Resolve3D20Fn g_orig_resolve3d20 = nullptr;
extern uint8_t* g_exe_base;     // defined below with the other engine globals
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugResolveHits = 0;
// ROOT CAUSE / ARCHITECTURAL FIX (layer 2 = deterministic version binding).
// The frame graph versions each logical resource; a consumer resolves "the current
// generation" of a logical id via the per-view declare counter v7 = *(a1+24064*salt+64)
// at the moment ITS declare runs. For MAIN the build order is fixed, so Final2D always
// declares AFTER tonemap's write => resolves the post-tonemap (final) generation. For
// the FORCED-DLSS vrcam view the parallel graph build is not deterministically ordered,
// so vrcam's Final2D declare lands before/after tonemap's write across frames => it
// resolves an EARLIER (pre/less-processed, brighter) generation on some frames and the
// final generation on others: the bright/normal flap.
// Fix: bind the consumer to the PRODUCER'S LATEST generation. In the resolver, track
// per (id, physical index) the generation it was resolved at; for vrcam's Final2D
// resolve of a post-color id, override the returned index to the recently-seen version
// with the MAXIMUM generation (= what the last writer produced). Recency window guards
// epoch changes. Post-color id = (salt<<24)^0x3D7E6258; salt only alters the top byte,
// so low 3 bytes 0x7E6258 match post-color for ANY view -> generalizes to both eyes.
constexpr uint32_t POSTCOLOR_LOW = 0x007E6258u;
// SAFETY DEFAULT 0: physical indices are PER-FRAME transient (proven: [pv] shows fin
// idx changing every frame at a FIXED gen=2), so substituting any remembered index =
// freed slot next frame = null deref (crashes 180201/180846). Override stays off until
// we identify a PERSISTENT correct index via the safe luma<->finidx correlation below.
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_FixPostVersion = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPinApplied = 0;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugFinNatural = 0;   // last natural fin idx
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugFinPinned  = 0;   // last pinned fin idx
// Tonemap (producer) OUTPUT: the index it resolves at the MAX generation this frame
// (= post-tonemap write, the dark/correct version). Captured read-only; consumed by
// Final2D by INDEX substitution -- which is crash-safe (match-tm ran stably for minutes,
// only bright because it used tonemap's LAST resolve = an input read, not the output).
static std::atomic<uint32_t> g_tm_post_idx{0};
static std::atomic<uint32_t> g_tm_post_gen{0};
static std::atomic<uint32_t> g_tm_post_seq{0};
static std::atomic<uint32_t> g_pv_seq{1};
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugTmPostIdx = 0;
// STAGE PROBE (path-A, 17): the vrcam post-color id 0x3C7E6258 is resolved at MANY
// pipeline STAGES (the resolver's a4/r9 salt = a stage index 36..71, live-confirmed).
// This maps every stage's resolved physical index per frame and dumps the whole map at
// the fin (mirror-blit) resolve, so we can diff DLSS on/off and see which stage's physical
// the fin consumer should read. Read-only; gated by CyberpunkVR_StageProbe (default 0).
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_StageProbe = 1;
static uint32_t g_stage_idx[128] = {0};   // salt(stage) -> last resolved physical index (vrcam 0x3C)
static uint32_t g_stage_idx_main[128] = {0}; // salt(stage) -> physical (MAIN 0x3D) for aliasing test
static uint32_t g_stage_v7 [128] = {0};   // salt(stage) -> gen counter at resolve
static std::atomic<uint32_t> g_stage_frame{0};
// WRITE-ORDER PROBE (root-cause): capture, per frame, the ORDER of vrcam post-color
// type-4 (WRITE) declares at salt70 with their generation + caller, then dump at the fin
// resolve tagged with fin's resolved index (bright/dark). Reveals whether the DLSS-raw
// write vs the composition/tonemap write lands LAST (highest gen) before Final2D reads,
// and whether that order races across bright/dark frames = the true root. Reliable C++
// gens (no x64dbg hex bug). Gated by CyberpunkVR_StageProbe. Declare hook on sub_1401F0F80.
constexpr uintptr_t DECLARE_RVA = 0x1F0F80;   // sub_1401F0F80
using DeclareFn = char(__fastcall*)(__int64, __int64, unsigned __int8, __int64);
static DeclareFn g_orig_declare = nullptr;
struct S70Write { uint32_t gen; uint32_t caller_rva; uint8_t type; };
static S70Write g_s70w[24] = {};
static volatile long g_s70w_n = 0;
struct S70Res { uint32_t caller_rva; uint32_t phys; };
static S70Res g_s70res[20] = {};
static volatile long g_s70res_n = 0;


// SAME-FRAME DE-ALIAS (path-A root fix, doc 23). Proven via [s70res]: within ONE frame the
// vrcam post-color is resolved by node377 at RET-RVA 0x378178 -> a STABLE correct physical
// (31691, vrcam's own), and by the DLSS temporal pass sub_140378224 at RET-RVA 0x3783CF ->
// the DISPLAYED physical that FLAPS (dark=31691 vrcam / bright=main-range, aliased from the
// shared transient pool). Both resolves are the SAME id in the SAME frame => both physicals
// are LIVE this frame => forcing the flapping (displayed) resolve to the stable one is
// CRASH-SAFE (unlike the cross-frame pin that used freed indices). Refreshed every frame
// (0x378178 is stable per scene), recency-guarded so a missed cache just falls back to
// natural (flap, no crash) rather than using a stale index.
constexpr uint32_t POST_STABLE_RET_RVA = 0x378178u;  // node377 stable post-color resolve
constexpr uint32_t POST_FLAP_RET_RVA   = 0x3783CFu;  // sub_140378224 resolve = displayed
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_DealiasPostColor = 1;   // default ON
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugDealiasHits = 0;
static std::atomic<uint32_t> g_post_stable_idx{0};
static std::atomic<uint32_t> g_post_stable_seq{0};
static std::atomic<uint32_t> g_dealias_seq{1};
static __int64 __fastcall Detour_Resolve3D20(__int64 reg, uint32_t* out,
        uint32_t* idp, __int64 r9) {
    const void* ra = _ReturnAddress();
    const __int64 r = g_orig_resolve3d20(reg, out, idp, r9);
    if (!out || !idp) return r;
    const uint32_t id = idp[0];
    if ((id & 0x00FFFFFFu) != POSTCOLOR_LOW) return r;      // post-color ids only
    // --- SAME-FRAME DE-ALIAS of vrcam post-color: cache node377's STABLE resolve (0x378178);
    // the DISPLAYED resolve is the mirror-blit consumer (in_fin, salt71) which flaps -> we
    // override THAT below to the cached stable physical (same-frame => live => crash-safe).
    if (id == 0x3C7E6258u) {
        const uint32_t rva = (uint32_t)(reinterpret_cast<uintptr_t>(ra)
            - reinterpret_cast<uintptr_t>(g_exe_base));
        const uint32_t s = g_dealias_seq.fetch_add(1, std::memory_order_relaxed);
        if (rva == POST_STABLE_RET_RVA) {
            g_post_stable_idx.store(*out, std::memory_order_release);
            g_post_stable_seq.store(s, std::memory_order_release);
        }
    }
    const bool in_fin = t_mirror_copy_node_active;          // vrcam Final2D consumer
    const bool in_tm  = t_vrcam_node_active && t_current_node_work ==
        reinterpret_cast<uintptr_t>(g_exe_base) + TONEMAP_WORK_RVA;
    // --- MAIN(0x3D) post-color: record per-salt physical to test main<->vrcam aliasing ---
    if (CyberpunkVR_StageProbe && id == 0x3D7E6258u) {
        const uint8_t st = (uint8_t)r9;
        if (st < 128) g_stage_idx_main[st] = *out;
    }
    // --- STAGE PROBE: record EVERY vrcam(0x3C) post-color resolve, all stages ---
    if (CyberpunkVR_StageProbe && id == 0x3C7E6258u) {
        const uint8_t st = (uint8_t)r9;
        uint32_t sv7 = 0;
        if (reg) { __try { sv7 = *reinterpret_cast<uint32_t*>(
            reg + 24064ull * st + 64); } __except (EXCEPTION_EXECUTE_HANDLER) { sv7 = 0; } }
        g_stage_idx[st] = *out;
        g_stage_v7[st]  = sv7;
        // salt70: record (caller-RVA -> resolved physical) this frame to compare the
        // WRITE resolve (node377 / sub_140378224) vs the READ resolve (Final2D 0x209xxx).
        if (st == 70) {
            const uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
            const uint32_t rva = (uint32_t)(ra - reinterpret_cast<uintptr_t>(g_exe_base));
            long i = _InterlockedIncrement(&g_s70res_n) - 1;
            if (i >= 0 && i < 20) { g_s70res[i].caller_rva = rva; g_s70res[i].phys = *out; }
        }
        if (in_fin) {
            // fin (mirror blit) runs late in the frame. Dump the salt70 WRITE-ORDER
            // (accumulated by Detour_Declare this frame) tagged with fin's resolved index,
            // so bright vs dark frames can be compared: which writer (caller) held the
            // LAST/highest gen before Final2D read = what determines the flap.
            const uint32_t fr = g_stage_frame.fetch_add(1, std::memory_order_relaxed);
            const long wn = g_s70w_n;
            if (fr < 600u) {                                 // cap total lines
                char buf[700]; int n = 0;
                n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE,
                    "[s70res] fr=%u finIdx=%u :", fr, *out);
                const long rn = g_s70res_n;
                for (long i = 0; i < rn && i < 20 && n < (int)sizeof(buf) - 32; ++i)
                    n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE,
                        " %X=%u", g_s70res[i].caller_rva, g_s70res[i].phys);
                log("%s", buf);
            }
            g_s70w_n = 0;                                    // reset for next frame
            g_s70res_n = 0;
        }
    }
    if (!in_fin && !in_tm) return r;                        // vrcam post nodes only
    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(
        &CyberpunkVR_DebugResolveHits));
    uint32_t v7 = 0;
    if (reg) {
        __try { v7 = *reinterpret_cast<uint32_t*>(reg + 24064ull * (uint8_t)r9 + 64); }
        __except (EXCEPTION_EXECUTE_HANDLER) { v7 = 0; }
    }
    const uint32_t seq = g_pv_seq.fetch_add(1, std::memory_order_relaxed);
    if (in_tm) {
        // Track tonemap's OUTPUT = its resolve at the MAX generation. gen is stable per
        // frame (~29) and recurs, so ">=" keeps latching the output; a thread-local
        // gen-drop check resets across frames.
        thread_local uint32_t tl_last_tm_gen = 0;
        uint32_t pg = g_tm_post_gen.load(std::memory_order_acquire);
        if (v7 + 4u < tl_last_tm_gen) pg = 0;              // gen dropped => new frame
        if (v7 >= pg) {
            g_tm_post_idx.store(*out, std::memory_order_release);
            g_tm_post_gen.store(v7, std::memory_order_release);
            g_tm_post_seq.store(seq, std::memory_order_release);
            CyberpunkVR_DebugTmPostIdx = *out;
        }
        tl_last_tm_gen = v7;
    } else {  // in_fin consumer (the mirror-blit / displayed resolve)
        CyberpunkVR_DebugFinNatural = *out;
        // SAME-FRAME DE-ALIAS: the displayed resolve flaps (dark=vrcam-own / bright=main
        // aliased). Force it to node377's stable resolve (0x378178) captured THIS frame
        // (same frame => live => crash-safe). Recency guard => fallback to natural on miss.
        if (CyberpunkVR_DealiasPostColor && id == 0x3C7E6258u) {
            const uint32_t si = g_post_stable_idx.load(std::memory_order_acquire);
            const uint32_t ss = g_post_stable_seq.load(std::memory_order_acquire);
            const uint32_t ds = g_dealias_seq.load(std::memory_order_relaxed);
            // Wide recency window: node377's 0x378178 resolve runs EVERY frame before the
            // mirror blit (proven: s70res missing-378178=0), so `si` is always same-frame
            // fresh -> a large window is safe (staleness only if 378178 stops for many
            // frames, e.g. DLSS off/menu, after which override self-disables). The async
            // per-frame variation in the resolve-count gap (was occasionally >48 -> rare
            // bright flash on static scenes) is covered here.
            if (si && si != *out && (ds - ss) < 512u) {
                *out = si;
                InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(
                    &CyberpunkVR_DebugDealiasHits));
            }
        }
        if (CyberpunkVR_FixPostVersion) {
            const uint32_t tmidx = g_tm_post_idx.load(std::memory_order_acquire);
            const uint32_t tmseq = g_tm_post_seq.load(std::memory_order_acquire);
            // Fresh (tonemap resolved this/last frame) => its index is current-frame
            // valid => crash-safe substitution (proven: match-tm never crashed).
            if (tmidx && (seq - tmseq) < 8u && tmidx != *out) {
                *out = tmidx;
                InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(
                    &CyberpunkVR_DebugPinApplied));
            }
        }
        CyberpunkVR_DebugFinPinned = *out;
    }
    { static std::atomic<uint32_t> lg{0};
      if (lg.fetch_add(1) < 60)
          log("[pv] %s id=%08X idx=%u v7=%u", in_fin ? "fin" : "tm ", id, *out, v7); }
    return r;
}
// Stable committed snapshot of the vrcam final. Filled INLINE inside the engine's own
// command list at the END of the vrcam RenderFinal2D node: after the composite draw is
// recorded and BEFORE any later pass records the ALIASING barrier that hands the
// transient's heap memory to another resource. Proven root of the bright/dark flicker:
// the deferred copy ran ~30 events AFTER that hand-off (ev95217 vs ev95245 in the
// 3-frame capture) and raced main's reuse of the shared heap (Heap_96361 @0) => read
// main's content on some frames. Committed => never aliased; rests in COMMON between
// frames; only the game queue touches it => no cross-queue hazard. The deferred mirror
// hop reads THIS instead of the transient. Later this is the OpenXR eye-submit surface.
static ID3D12Resource*     g_stable_tex = nullptr;
static D3D12_RESOURCE_DESC g_stable_desc{};
static std::mutex          g_stable_mtx;
static std::atomic<bool>   g_stable_fresh{false};
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_StableCopy = 1;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugStableCopies = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugStableSkips = 0;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugStableSrcState = 0;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_MirrorOutput;  // defined below

// ---- who wants the VRCAM colour snapshot -----------------------------------------------
// The snapshot used to exist purely for the desktop mirror, so its capture was gated on
// MirrorOutput. The OpenXR submit now reads the SAME resource for the right eye, and it must
// be able to do so with the mirror window closed -- that window is a capture convenience,
// not a prerequisite for stereo.
extern "C" __declspec(dllexport) int32_t CyberpunkVR_StereoEyeCapture = 1;
static inline bool stereo_eye_capture_wanted() {
    return CyberpunkVR_MirrorOutput != 0 || CyberpunkVR_StereoEyeCapture != 0;
}
// When the last snapshot was taken. The submit needs LIVENESS, not existence: g_stable_fresh
// latches true on the first copy and never clears, so on its own it would keep handing the
// right eye a frozen image after the second view stops (menu, component off) -- one eye
// live and one eye stuck is far worse than plain mono.
static std::atomic<uint64_t> g_stable_tick{0};
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_StereoEyeMaxAgeMs = 250;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugVrcamEyeAgeMs = 0xFFFFFFFFu;
// THE FOUR PRECONDITIONS OF THE SECOND EYE, COUNTED SEPARATELY.
//
// g_stable_tick above is the clock the submit reads: older than StereoEyeMaxAgeMs and
// GetVrcamEyeTextureFresh hands back null, which is mono. Everything that can stop that clock
// happens in one node epilogue, and until now every one of those exits was silent -- which is how
// the last watchdog managed to stay quiet while the eye died. It sits inside mirror_publish_output,
// so it could only ever report on frames where that function RAN; the snapshot needs two things
// publish does not (the engine's command list, and a hook entry for it), and a failure there is
// invisible from in there.
//
// So each step gets its own counter and the report is printed where the decision is actually made.
static std::atomic<uint64_t> g_eye_node_hits{0};   // the vrcam CopyToTexture node ran
static std::atomic<uint64_t> g_eye_no_rtv{0};      // ...but no output RTV was latched in it
static std::atomic<uint64_t> g_eye_no_list{0};     // ...RTV yes, but no command list to record on
static std::atomic<uint64_t> g_eye_copy_calls{0};  // the snapshot copy was actually attempted
// Luma oscilloscope: turn the bright/normal alternation into NUMBERS. Each vrcam frame
// the deferred hop also copies an 8x8 center block of the stable snapshot into a
// readback slot (ringed with the existing copy fence); when a slot is reused its
// completed block is decoded (R11G11B10F) and average luma is accumulated (a) by frame
// parity and (b) as mean |dL| between CONSECUTIVE frames (parity-proof oscillation
// amplitude). One log line per ~120 vrcam frames -> fixes get A/B'd by numbers.
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_LumaProbe = 1;   // diagnostics OFF (flicker RE done) -> no readbacks / [luma][cbflap][chain]... log spam
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugLumaEvenMilli = 0;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugLumaOddMilli = 0;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugLumaDeltaMilli = 0;
// Waveform: log the first N per-frame luma samples ("[lumaw] fr=.. L=..") so the
// oscillation SHAPE is visible (strict period-2 square = history ping-pong misread;
// multi-frame sawtooth/pulse = exposure-adaptation feedback limit cycle). Decrements
// per line; re-arm live by writing a new count via x64dbg.
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_LumaWave = 300;
// CB-flap probe: the vrcam tonemap pass (the [2rt] bind) gets an 848-byte constant
// buffer uploaded right after its bind. Capture that CB resource+offset, read it back
// every frame alongside the luma sample, and auto-detect dwords that take EXACTLY two
// values and flip in sync with the bright/normal luma state -> that dword IS the raced
// exposure input, and its two bit-patterns identify the source variable in memory.
thread_local bool t_in_vrcam_2rt = false;   // between vrcam 2-RT bind and next OMSet
thread_local bool t_2rt_cb_armed = false;   // capture only the FIRST 848B upload
// There are SEVERAL vrcam 2-RT passes per frame; only the TONEMAP's RT1 descriptor
// ping-pongs across frames (persistent history pair), the others rebind the same RT1
// handle every frame. Identify the tonemap by observing an RT1 change for a given RT0
// (lock-free: hk_OM contains __try => no C++ destructors allowed => no mutexes here).
static std::atomic<SIZE_T> g_2rt_seen_h0[4] = {};
static std::atomic<SIZE_T> g_2rt_seen_h1[4] = {};
static std::atomic<SIZE_T> g_tonemap_h0{0};
static std::atomic<ID3D12Resource*> g_cb_res{nullptr};
static std::atomic<uint64_t>        g_cb_off{0};
static std::atomic<ID3D12Resource*> g_cb_last_res{nullptr};
static std::atomic<bool>            g_cb_reset_pending{false};
static ID3D12Resource* g_cb_rb[4] = {};     // READBACK, persistently mapped
static uint8_t*        g_cb_map[4] = {};
static bool            g_cb_valid[4] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCbCaptures = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugFrameCb = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugHudCb = 0;
// Set by the composite's own validity check: the engine's b6 was bound and its target size
// matched ours, so every constant came from the engine rather than from the capture.
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugHudCbUsed = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_Debug2RtBinds = 0;
static ID3D12Resource* g_luma_rb[4] = {};      // READBACK heap, persistently mapped
static uint8_t*        g_luma_map[4] = {};
static uint32_t        g_luma_parity[4] = {};
static uint32_t        g_luma_frame[4] = {};
static uint32_t        g_luma_finidx[4] = {};   // fin natural index at capture (safe correlation)
static bool            g_luma_valid[4] = {};
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugFinNatural;  // set in resolver
static void mirror_stable_inline_copy(ID3D12GraphicsCommandList*, ID3D12Resource*,
        uint32_t src_state);
static void mirror_publish_output(ID3D12Resource*, DXGI_FORMAT);
// Master gate for the OLD eye-X build path (Path 1a: own empty manager -> FinalOnly
// -> OOM). Disabled so `sync_typea_on` runs ONLY the new option-B context-inject
// (2nd context in main's manager) in isolation for testing.
bool g_enable_eye_x_build = false;
#ifdef TESTBED_EARLY_REG_PROBE
bool g_enable_native_registration_probe = true;
#else
bool g_enable_native_registration_probe = false;
#endif
constexpr uintptr_t TYPE_A_JOB_VT_RVA      = 0x2ABC878;
constexpr uintptr_t TYPE_B_JOB_VT_RVA      = 0x2AC8668;
constexpr uintptr_t SPEC_EMPTY_INIT_RVA    = 0x2933C4;
constexpr uintptr_t SPEC_COPY_INIT_RVA     = 0x24EB78;
constexpr uintptr_t SPEC_EMPTY_DTOR_RVA    = 0x25214C;
constexpr uintptr_t BUILD_MATE_INIT_RVA    = 0x29112C;
constexpr uintptr_t BUILD_MATE_FILL_RVA    = 0x252034;
constexpr uintptr_t VIEW_PRODUCER_RVA      = 0x293978;
constexpr uintptr_t BUILD_MATE_DTOR_RVA    = 0x28CF20;
constexpr uintptr_t VIEW_SPEC_DTOR_RVA     = 0x28BD2C;
constexpr uintptr_t SPEC_FINAL_BIND_RVA    = 0x28C298;

// CALLER1 prologue, 22 fixed bytes (extracted from the live exe):
//   mov [rsp+10h],rbx ; push rbp/rsi/rdi ; lea rbp,[rsp-16B0h] ;
//   mov eax,17B0h ; call __chkstk
// The chkstk call displacement that follows the trailing E8 is position-
// dependent, so the pattern STOPS at the E8 opcode  everything before is
// invariant for this function.
const uint8_t kCaller1Pat[] = {
    0x48,0x89,0x5C,0x24,0x10, 0x55,0x56,0x57, 0x48,0x8D,0xAC,0x24,0x50,0xE9,0xFF,0xFF,
    0xB8,0xB0,0x17,0x00,0x00, 0xE8
};

const uint8_t kFlushRenderScenePat[] = {
    0x48,0x89,0x5C,0x24,0x20, 0x55,0x56,0x57, 0x48,0x8B,0xEC,
    0x48,0x81,0xEC,0x80,0x00,0x00,0x00, 0x48,0x8B,0xFA,
    0x48,0x8B,0xD9, 0xB2,0x01, 0x48,0x8D,0x4D,0xC8,
    0x45,0x33,0xC0, 0xE8
};

const uint8_t kCameraWritePat[] = {
    0x48,0x8B,0xC4, 0x48,0x89,0x58,0x08, 0x48,0x89,0x70,0x10,
    0x48,0x89,0x78,0x18, 0x4C,0x89,0x70,0x20, 0x55,
    0x48,0x8D,0xA8,0xE8,0xF4,0xFF,0xFF, 0x48,0x81,0xEC,0x10,0x0C
};

const uint8_t kFrameGatePat[] = {
    0x48,0x8B,0x49,0x20, 0xE9,0x7B,0x02,0x00,0x00
};

const uint8_t kFgBuildPat[] = {
    0x48,0x89,0x5C,0x24,0x08, 0x57,
    0x48,0x81,0xEC,0x80,0x00,0x00,0x00,
    0x4C,0x8B,0x4A,0x20, 0x48,0x8B,0xD9,
    0xBF,0x9A,0x01,0x00,0x00, 0x4D,0x85,0xC9,
    0x74,0x17, 0x65,0x48
};

const uint8_t kRunRenderNodesPat[] = {
    0x48,0x89,0x54,0x24,0x10, 0x48,0x89,0x4C,0x24,0x08,
    0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57
};

const uint8_t kRunNodeBatchSubmitPat[] = {
    0x48,0x8B,0xC4, 0x48,0x89,0x58,0x08, 0x48,0x89,0x70,0x10,
    0x57, 0x48,0x83,0xEC,0x50, 0x83,0x60,0xF0,0x00
};

const uint8_t kRunNodeBatchWorkPat[] = {
    0x48,0x89,0x5C,0x24,0x08, 0x48,0x89,0x74,0x24,0x10,
    0x48,0x89,0x7C,0x24,0x18, 0x41,0x56, 0x48,0x81,0xEC,0x80,0x00,0x00,0x00
};

const uint8_t kGraphRequestBuildPat[] = {
    0x48,0x83,0xEC,0x58, 0x48,0x8B,0x51,0x28,
    0x4C,0x8D,0x89,0xD0,0x03,0x00,0x00,
    0x4C,0x8D,0x41,0x30, 0x0F,0x57,0xC0, 0x48,0x8B,0x49,0x20
};

const uint8_t kGraphContextPreparePat[] = {
    0x48,0x89,0x5C,0x24,0x18, 0x48,0x89,0x54,0x24,0x10,
    0x55,0x56,0x57, 0x41,0x54, 0x41,0x55, 0x41,0x56, 0x41,0x57,
    0x48,0x8D,0xAC,0x24,0x30,0xFC,0xFF,0xFF,
    0x48,0x81,0xEC,0xD0,0x04,0x00,0x00
};

const uint8_t kGraphRequestRegisterPat[] = {
    0x48,0x89,0x5C,0x24,0x10, 0x48,0x89,0x6C,0x24,0x18,
    0x56,0x57,0x41,0x56, 0x48,0x83,0xEC,0x30,
    0x48,0x8D,0x99,0x20,0x01,0x00,0x00,
    0x48,0x8B,0xEA, 0x4C,0x8D,0xB1,0x28
};


// CALLER1 ABI (from SYNC_SEQUENTIAL_PROVEN): rcx=mgr, xmm1=float, then ptrs.
// LIGHT (sub_14029A5B0) uses the identical ABI per Q1 of the round-6 audit.
using NodeDispatchFn = uint8_t (__fastcall *)(uintptr_t* node, uint8_t* work_context, void* args);

// Camera-writer sub_140788A9C (RVA 0x788A9C): writes view/proj into the
// view-state. Prologue 32 fixed bytes (no relative calls). Returns bool in al.
constexpr uintptr_t CAMW_RVA = 0x788A9C;
const uint8_t kCamwPat[] = {
    0x48,0x8B,0xC4, 0x48,0x89,0x58,0x08, 0x48,0x89,0x70,0x10, 0x48,0x89,0x78,0x18,
    0x4C,0x89,0x70,0x20, 0x55, 0x48,0x8D,0xA8,0xE8,0xF4,0xFF,0xFF, 0x48,0x81,0xEC,0x10,0x0C
};
using CamwFn = uint8_t (*)(void* rcx, void* rdx, void* r8, void* r9);

// View translation X lives at view-state +0x150 (camera space). Patching it
// laterally shifts the rendered image  the IPD lever (single float).
constexpr uintptr_t OFF_VIEW_TX = 0x150;

// mgr scalar offsets (round-6 SUBMIT mutation surface). +0x118 is the frame
// call counter; SUBMIT-fn (called by BOTH full and light) increments it.
constexpr uintptr_t OFF_FRAME_CTR = 0x118;  // dword
constexpr uintptr_t OFF_S_15C     = 0x15C;  // dword
constexpr uintptr_t OFF_S_188     = 0x188;  // dword
constexpr uintptr_t OFF_S_18C     = 0x18C;  // dword
constexpr uintptr_t OFF_B_1C0     = 0x1C0;  // byte (view-active flag)
constexpr uintptr_t OFF_QPC_2B8   = 0x2B8;  // qword (QPC timestamp)
constexpr uintptr_t OFF_S_348     = 0x348;  // dword (CALLER1-only scalar)

// Renderer global + view-state (render_camera_RE/STATE.md, verified addresses).
// renderer = *(qword_143427C00); shared view-state = renderer + 0x4658.
constexpr uintptr_t RVA_RENDERER_GLOBAL = 0x3427C00;  // qword_143427C00
constexpr uintptr_t OFF_VIEWSTATE       = 0x4658;     // renderer + this
constexpr size_t VIEW_STATE_SNAPSHOT_OFFSET = 0x20;
constexpr size_t VIEW_STATE_SNAPSHOT_SIZE = 0x488;    // through last-frame token at +0x4A0
// Never wait for Present while holding the FG serialization lock. The 100 ms
// diagnostic barrier collapses stereo throughput to roughly 10 FPS whenever
// the offscreen/right graph intentionally has no desktop Present.
constexpr bool HAS_DXGI_PRESENT_OBSERVER = false;

uint8_t*               g_exe_base   = nullptr;

// Retained globals still referenced by the kept node dispatcher, record_node_dispatch and init
// (the rest of the old Type-A globals block was orphaned and removed).
using WaitOnAddressFn = BOOL (WINAPI *)(volatile VOID*, PVOID, SIZE_T, DWORD);
using WakeByAddressAllFn = VOID (WINAPI *)(PVOID);
WaitOnAddressFn         g_wait_on_address = nullptr;
WakeByAddressAllFn      g_wake_by_address_all = nullptr;
NodeDispatchFn          g_node_dispatch_orig = nullptr;
std::atomic<bool>       g_node_dispatch_hooked{false};

static bool is_vrcam_copy_to_texture(uintptr_t* node, uint8_t* work_context) {
    if (!node || !work_context) return false;
    __try {
        const uintptr_t ctx = *reinterpret_cast<uintptr_t*>(work_context + 0x18);
        if (!ctx || *reinterpret_cast<uint64_t*>(ctx + 0x28) !=
                g_vrcam_ctx_key) {
            return false;
        }
        const uintptr_t vtable = *node;
        const uintptr_t work = *reinterpret_cast<uintptr_t*>(vtable + 8);
        const bool hit = work == reinterpret_cast<uintptr_t>(g_exe_base) +
            RENDER_FINAL2D_WORK_RVA;
        if (hit) ++CyberpunkVR_DebugMirrorCopyNodeHits;
        return hit;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- per-node CPU audit (phase 0) ------------------------------------------
// Successor of the removed record_node_dispatch audit: instead of exec ORDER it
// accumulates per-work-fn CPU TIME, split main vs vrcam. Open-addressing table
// keyed by work-fn RVA; dumped+reset on demand (overlay button) to cyberpunkvrport_stereo.log.
// Identifies exactly which node types cost CPU twice for simultaneous stereo.
struct ProfNode {
    std::atomic<uintptr_t> rva;         // work-fn RVA (0 = empty slot)
    std::atomic<int64_t>   ticks_main;  // INCLUSIVE: this node + the nodes it dispatches
    std::atomic<int64_t>   ticks_vrcam;
    std::atomic<int64_t>   self_main;   // EXCLUSIVE: inclusive minus its child dispatches
    std::atomic<int64_t>   self_vrcam;
    std::atomic<uint32_t>  calls_main;
    std::atomic<uint32_t>  calls_vrcam;
    std::atomic<uint32_t>  ord_main;    // 1-based first-seen dispatch order (0 = never seen)
    std::atomic<uint32_t>  ord_vrcam;
};
static ProfNode g_prof_nodes[512];      // ~few hundred distinct work fns in practice
// First-seen order counters. The graph is stable frame to frame, so first-seen order ==
// dispatch order, which is what makes the dump diffable against the older audits.
static std::atomic<uint32_t> g_prof_ord_main{0};
static std::atomic<uint32_t> g_prof_ord_vrcam{0};
static void prof_node_add(uintptr_t work, int64_t dt, int64_t self, bool vrcam) {
    if (!work || !g_exe_base) return;
    const uintptr_t rva = work - reinterpret_cast<uintptr_t>(g_exe_base);
    uint32_t h = static_cast<uint32_t>((rva >> 4) * 2654435761u);
    for (int probe = 0; probe < 16; ++probe) {
        ProfNode& n = g_prof_nodes[(h + probe) & 511];
        uintptr_t cur = n.rva.load(std::memory_order_relaxed);
        if (cur == 0) {
            uintptr_t expected = 0;
            if (n.rva.compare_exchange_strong(expected, rva)) cur = rva;
            else cur = expected;                  // lost race: someone claimed it
        }
        if (cur == rva) {
            if (vrcam) {
                n.ticks_vrcam.fetch_add(dt, std::memory_order_relaxed);
                n.self_vrcam.fetch_add(self, std::memory_order_relaxed);
                if (n.calls_vrcam.fetch_add(1, std::memory_order_relaxed) == 0)
                    n.ord_vrcam.store(g_prof_ord_vrcam.fetch_add(1, std::memory_order_relaxed) + 1,
                                      std::memory_order_relaxed);
            } else {
                n.ticks_main.fetch_add(dt, std::memory_order_relaxed);
                n.self_main.fetch_add(self, std::memory_order_relaxed);
                if (n.calls_main.fetch_add(1, std::memory_order_relaxed) == 0)
                    n.ord_main.store(g_prof_ord_main.fetch_add(1, std::memory_order_relaxed) + 1,
                                     std::memory_order_relaxed);
            }
            return;
        }
    }
}
// SceneDrv (sub_1401EC1D0) is the SINGLE work-fn behind ALL scene-geometry passes; the pass
// id is the rtId byte at exec-ctx+0x38. CAUTION: rtId is NOT a stable pass identity -- the
// graph builder hands out the NEXT SEQUENTIAL index per top-level pass scope
// (sub_1409853B4 opens, sub_141321968 closes), so one pass added or skipped earlier
// RENUMBERS the whole tail. MAIN and VRCAM therefore disagree on numbering (a constant
// shift of 3 in the tail was observed), and comparing views by rtId NUMBER produces phantom
// "view-only passes". Passes must be matched by CONTENT instead -- hence the pair table below.
static const uint32_t SCENE_DRIVER_WORK_RVA = 0x1EC1D0;

// ---- HUD IN THE SECOND EYE ------------------------------------------------------------------
//
// CRenderNode_DrawHUD (work sub_1401EE760) opens with a per-view capability test, not with
// anything HUD-specific:
//
//     cmp  [rdx+18h], rsi        ; no view ctx at all ->
//     jz   draw                  ;   draw unconditionally
//     cmp  [rdx+20h], rsi
//     jz   draw
//     lea  rdx, word_143487820   ; the feature descriptor this node requires
//     call sub_14021BE28         ; does THIS view have those bits?
//     test al, al
//     jz   epilogue              ; <- no: return without drawing
//
// and sub_14021BE28 is a plain subset test over the view's own capability bitmask, 32 qwords at
// view+0x18A0 (6304), against a 0x110-byte descriptor from a table of them:
//
//     while (required[i] & viewBits[i]) == required[i]) if (++i >= 0x20) return 1;
//     return 0;
//
// So the second eye is not being refused by anything to do with the HUD -- it simply does not
// carry that capability bit, because a render-to-texture camera is an engine feature meant for
// mirrors and surveillance monitors, where a HUD would be wrong.
//
// This is NOT the component's `features` field (entRenderToTextureFeatures, 8 bytes: decals,
// particles, forwardNoTXAA, AA, contact shadows, local shadows, SSAO, reflections). That one is
// a handful of quality switches; the mask tested here is 2048 bits assembled by the view
// producer. Setting `features` cannot reach it.
//
// The narrowest correct intervention is therefore to answer that one question differently, and
// only for the second eye, and only while the HUD node is the one asking.
// RenderFinal2D is NOT part of this problem: it already runs for the second view -- the whole
// right-eye capture hangs off it (RENDER_FINAL2D_WORK_RVA above, the ctx-keyed RTV redirect).
// So the composite that would put a HUD surface on screen is present; only the node that DRAWS
// the HUD is being refused.
static const uint32_t DRAWHUD_WORK_RVA        = 0x1EE760;   // CRenderNode_DrawHUD
static const uintptr_t VIEW_FEATURE_CHECK_RVA = 0x21BE28;   // sub_14021BE28

// OFF -- forcing the gate CRASHES, and that is the useful result.
//
// With the refusal overridden the node ran on and immediately took an access violation reading
// 0xF0 off a null pointer (report 20260728-105606; our "capability refusal overridden" line is
// the last thing in the log before it). So the capability test is not an arbitrary veto: it is
// the engine declining to run a node whose prerequisites this view does not have. Skipping the
// question does not create the answer -- behind it there is no 2D/HUD state on an RTT view, and
// past the first missing pointer there would only be the next one.
//
// The way in is therefore to SATISFY the condition, not remove it: find which capability bits
// the node requires, find which of them the second view lacks, and set them where the view is
// built -- early enough that the engine itself allocates everything that follows. The mask dump
// below is the measurement that makes that possible.
extern "C" __declspec(dllexport) int CyberpunkVR_HudInVrcam = 0;
// Dispatch census: does the node reach the second view AT ALL? If Vrcam stays 0 while Main
// climbs, the node is not in that view's graph and the capability test is not the obstacle --
// a different problem, and this hook cannot fix it.
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugHudNodeMain    = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugHudNodeVrcam   = 0;
// How often the engine refused the second eye, and how often we overrode that refusal.
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugHudGateDenied  = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugHudGateForced  = 0;

using ViewFeatureCheckFn = uint8_t (__fastcall*)(uintptr_t work_context, uintptr_t required);
static ViewFeatureCheckFn g_view_feature_check_orig = nullptr;

// ---- THE SECOND GATE: the view's draw-block list ---------------------------------------------
//
// Past the capability test, DrawHUD immediately does:
//
//     1401EE810  mov  rcx, rdi            ; work_context
//     1401EE813  call sub_1401ED930       ; -> viewData
//     1401EE823  mov  r15, [r14+168h]     ; the view's draw-block list
//     1401EE83E  test r15, r15
//     1401EE841  jz   loc_1401F00EA       ; empty -> return without drawing
//
// The counters said this is where it stops: `denied` came out EXACTLY equal to the VRCAM
// dispatch count, and DrawHUD asks the capability question twice. One refusal per call means the
// node never reached the second question -- it left in between, and this is the only exit there.
//
// viewData+0x168 is not a new discovery either: Detour_DrawComposition already reads that slot,
// already calls it the block list, and already lends MAIN's to VRCAM under CullReuseMode 5. So
// the mechanism is known-good; it just was never applied to the HUD node.
//
// Borrowed and RESTORED around the call, never assigned: the slot belongs to the engine's view
// object, and leaving a foreign pointer in it after the node returns would hand MAIN's list to
// whatever runs next on that view.
using HudViewDataFn = void* (__fastcall*)(void*);
static HudViewDataFn g_hud_viewdata_get = nullptr;
static void* g_hud_block_main = nullptr;      // MAIN's list, captured at MAIN's own DrawHUD
// OFF for the same reason as the gate override: lending the list only carried the node further
// into state the view does not have. Kept because the measurement it produces (null vs ok) is
// still worth having, and because it becomes correct once the capability bits are set properly.
extern "C" __declspec(dllexport) int CyberpunkVR_HudBorrowBlocks = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugHudBlockNull    = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugHudBlockOk      = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugHudBlockLent    = 0;

// ---- WHAT EXACTLY THE SECOND VIEW IS MISSING -------------------------------------------------
//
// The capability test compares a 32-qword requirement descriptor against the view's own 32-qword
// bitmask, and refuses when the view does not contain every required bit:
//
//     required = word_143487820 + 8      (the descriptor's payload starts one qword in)
//     viewBits = ctx + 6304              (= view + 0x18A0)
//     for i in 0..31: if ((required[i] & viewBits[i]) != required[i]) -> refuse
//
// Note the OTHER flag block we already manipulate, at ctx + 6096, is NOT this one -- that is the
// frame-graph build word pair (f0/f1) the [fgflags] path forces. 208 bytes apart, different
// purpose, and it was never going to reach this test.
//
// Dumped ONCE for MAIN and ONCE for VRCAM, plus the per-qword delta, so the answer is a list of
// bit positions rather than a theory. Those positions are what has to be set where the view is
// created -- early enough that the engine allocates the HUD state itself, which is the whole
// difference between this and the override that crashed.
static const uintptr_t HUD_REQUIRED_MASK_RVA = 0x3487820;   // word_143487820
static bool g_hud_mask_dumped_main  = false;
static bool g_hud_mask_dumped_vrcam = false;

// ---- GRANT THE CAPABILITY INSTEAD OF SKIPPING THE TEST ---------------------------------------
//
// Measured, the second view is short of exactly ONE bit of what DrawHUD asks for:
//
//     MAIN  w11 req=...0080 have=0000000000CFFFBF missing=0
//     VRCAM w11 req=...0080 have=0000000000CF6E3F missing=0000000000000080
//
// word 11, bit 7 -- absolute feature 711. (The full difference between the two views in that
// word is 0x9180, bits 7/8/12/15; the HUD needs only the first.) And note this has nothing to do
// with the f0/f1 pair the [fgflags] path forces: VRCAM's f0/f1 are already a superset of MAIN's
// (3C00017F vs 3C00017D), which is why FORCED never changed anything.
//
// Why setting the bit is not the same thing as the override that crashed: the override answered
// one question at the moment it was asked, deep inside the node, long after the work that
// question guards was skipped. The bit is what the rest of the engine READS -- including,
// evidently, whatever collects HUD elements, since VRCAM's draw-block list comes out empty on
// every single frame (blocks null == vrcam dispatch count, exactly). Set early and left set, the
// view genuinely declares the capability and the engine populates the state itself.
//
// Whether that is enough is a measurement, not a claim: if the block list stops being null, the
// collection ran and this was the right lever. If it stays null, collection is gated somewhere
// else and this bit is only the last of several conditions.
//
// RESULT: it was necessary but not sufficient. `denied` went to 0 with no crash -- the view now
// passes the test honestly -- but the draw-block list stays empty on every frame.
//
// And the obvious explanation is WRONG, so do not reach for it again: both views are built by the
// SAME builder. The log says so directly ("VRCAM built via FULL (sub_141D43040)"), and VRCAM's
// f0/f1 are a superset of MAIN's. Same graph, same nodes; the difference is in the DATA the
// nodes find, not in which nodes exist. So the empty list has a writer that runs for one view and
// not the other, and finding that writer is the next step -- not re-litigating the builder.
extern "C" __declspec(dllexport) int      CyberpunkVR_HudGrantCap  = 1;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_HudCapWord   = 11;      // qword index
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_HudCapBits   = 0x80;    // bit 7 (=711)
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugHudCapGrants = 0;

// Idempotent OR into the view's own capability mask. Deliberately NOT saved/restored: a
// capability that only exists while one node runs is exactly the half-measure that crashed.
static void hud_grant_capability(uintptr_t ctx) {
    if (!CyberpunkVR_HudGrantCap || !ctx) return;
    const uint32_t w = CyberpunkVR_HudCapWord;
    const uint64_t bits = CyberpunkVR_HudCapBits;
    if (w >= 32 || !bits) return;
    __try {
        uint64_t* slot = reinterpret_cast<uint64_t*>(ctx + 6304) + w;
        if ((*slot & bits) != bits) {
            *slot |= bits;
            InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(
                &CyberpunkVR_DebugHudCapGrants));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void hud_dump_capability_mask(uint8_t* work_context, bool vrcam) {
    if (vrcam ? g_hud_mask_dumped_vrcam : g_hud_mask_dumped_main) return;
    if (!work_context || !g_exe_base) return;
    if (!g_hud_viewdata_get) {
        g_hud_viewdata_get = reinterpret_cast<HudViewDataFn>(g_exe_base + 0x1ED930);
    }
    __try {
        const uintptr_t ctx = *reinterpret_cast<uintptr_t*>(work_context + 0x18);
        if (!ctx) return;
        const uint64_t* viewBits = reinterpret_cast<const uint64_t*>(ctx + 6304);
        const uint64_t* required =
            reinterpret_cast<const uint64_t*>(g_exe_base + HUD_REQUIRED_MASK_RVA + 8);
        if (vrcam) g_hud_mask_dumped_vrcam = true; else g_hud_mask_dumped_main = true;

        int missingWords = 0;
        for (int i = 0; i < 32; ++i) {
            const uint64_t req = required[i];
            const uint64_t have = viewBits[i];
            if (!req && !have) continue;
            const uint64_t missing = req & ~have;
            if (req || missing) {
                log("[hudmask] %s w%02d req=%016llX have=%016llX missing=%016llX",
                    vrcam ? "VRCAM" : "MAIN ", i,
                    (unsigned long long)req, (unsigned long long)have,
                    (unsigned long long)missing);
            }
            if (missing) ++missingWords;
        }
        log("[hudmask] %s summary: qwords with missing bits = %d (0 means this view would pass)",
            vrcam ? "VRCAM" : "MAIN ", missingWords);

        // Addresses for a hardware write-breakpoint on the block-list slot. Both views are built
        // by the same builder, so whatever fills viewData+0x168 for MAIN runs for VRCAM too and
        // bails on some condition -- and a write watchpoint on MAIN's slot names it in one shot,
        // which no amount of reading the graph will.
        void* vd = g_hud_viewdata_get ? g_hud_viewdata_get(work_context) : nullptr;
        void* blocks = nullptr;
        if (vd) blocks = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(vd) + 0x168);
        const uint32_t layerTag = *reinterpret_cast<uint32_t*>(work_context + 0x14);
        log("[hudmask] %s ctx=%p viewData=%p blocks(+0x168)=%p watch=%p layerTag=0x%02X",
            vrcam ? "VRCAM" : "MAIN ",
            reinterpret_cast<void*>(ctx), vd, blocks,
            vd ? reinterpret_cast<void*>(reinterpret_cast<uint8_t*>(vd) + 0x168) : nullptr,
            layerTag);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
struct ProfPass {
    std::atomic<int64_t>  ticks_main;
    std::atomic<int64_t>  ticks_vrcam;
    std::atomic<uint32_t> calls_main;
    std::atomic<uint32_t> calls_vrcam;
};
static ProfPass g_prof_scenepass[256];   // full 0..255: 0xFF is a legitimate rtId value

// (rtId, work-fn) pair table: which NAMED nodes ran under which pass, per view. This is what
// makes the pass list comparable across views without knowing a pass name at all -- a pass is
// identified by the multiset of nodes inside it.
// ctx+0x38 is safe as the key: both ctx ctors (sub_1401ECBA0/sub_1401ECA90) init it to 0xFF,
// sub_1401ECF2C sets it from the node descriptor right before the runner dispatches, the
// copy-ctor sub_1401EC7EC propagates it, and the driver never writes any ctx field -- so it is
// constant for a whole pass and inherited by children. Keying on it beats a thread-local
// "enclosing pass" stack, because job-batched children can run on a worker thread at depth 0
// while still carrying the correct inherited rtId.
// Depth is a COLUMN, not part of the key: for a top-level node the row means "this node's own
// ctx slot", not "ran inside pass N".
struct ProfPair {
    std::atomic<uint64_t> key;          // (rtId << 32) | work-fn RVA; 0 = empty slot
    std::atomic<int64_t>  self_main;
    std::atomic<int64_t>  self_vrcam;
    std::atomic<uint32_t> calls_main;
    std::atomic<uint32_t> calls_vrcam;
    std::atomic<uint32_t> nested_main;  // dispatched below depth 0 => really inside a pass
    std::atomic<uint32_t> nested_vrcam;
    std::atomic<uint32_t> owner_main;   // dispatches with the node-arg owner bit (+0x30 & 2)
    std::atomic<uint32_t> owner_vrcam;
};
static ProfPair g_prof_pairs[2048];     // ~400 live rows expected (363 main / 320 vrcam)
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugProfPairOverflow = 0;
// splitmix64 finaliser: the slot index must depend on the WHOLE key. A plain
// `(key >> 4) * 2654435761` does not work here -- the table index uses the LOW bits of the
// product, and those depend only on the low bits of the input, i.e. on rva alone. rtId
// (bits 32+) never reached the index, so every pass sharing a node started at the same slot;
// RenderElements alone appears under ~30 rtIds, which blew past the probe limit and dropped
// 592062 dispatches in the first capture while showing only 404 rows.
static inline uint32_t prof_pair_hash(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (uint32_t)x;
}
static void prof_pair_add(uint8_t rtid, uint32_t rva, int64_t self, bool vrcam,
                          bool nested, bool owner) {
    if (!rva) return;
    const uint64_t key = ((uint64_t)rtid << 32) | rva;
    const uint32_t h = prof_pair_hash(key);
    for (int probe = 0; probe < 64; ++probe) {
        ProfPair& p = g_prof_pairs[(h + probe) & 2047];
        uint64_t cur = p.key.load(std::memory_order_relaxed);
        if (cur == 0) {
            uint64_t expected = 0;
            cur = p.key.compare_exchange_strong(expected, key) ? key : expected;
        }
        if (cur != key) continue;
        if (vrcam) {
            p.self_vrcam.fetch_add(self, std::memory_order_relaxed);
            p.calls_vrcam.fetch_add(1, std::memory_order_relaxed);
            if (nested) p.nested_vrcam.fetch_add(1, std::memory_order_relaxed);
            if (owner)  p.owner_vrcam.fetch_add(1, std::memory_order_relaxed);
        } else {
            p.self_main.fetch_add(self, std::memory_order_relaxed);
            p.calls_main.fetch_add(1, std::memory_order_relaxed);
            if (nested) p.nested_main.fetch_add(1, std::memory_order_relaxed);
            if (owner)  p.owner_main.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    ++CyberpunkVR_DebugProfPairOverflow;   // table full: rows would be lost, dump says so
}
// Work-fn RVA -> CRenderNode name, generated from the project's own RE census
// (engine_re/dumps/nodes/nodes_index.md, 163 nodes). This REPLACES the old hand-written
// switch, several entries of which were wrong guesses: +0x3726CC was labelled
// "ShadowCascades?" but is RenderRainMap, +0x378E68 "ShadowFamily?" is
// PrepareScreenSpaceWaterDepth, +0xA9B0F4 "AsyncRenderJob" is FlushTextureGrabs, and
// +0x768510 "RasterTonemap" is ApplyTXAA. Table is sorted by RVA -> binary search.
#include "node_names.inc"
extern "C" __declspec(dllexport) const char* CyberpunkVR_ProfNodeName(uint32_t rva) {
    int lo = 0, hi = (int)(sizeof(k_prof_node_names) / sizeof(k_prof_node_names[0])) - 1;
    while (lo <= hi) {
        const int mid = lo + ((hi - lo) >> 1);
        const uint32_t m = k_prof_node_names[mid].rva;
        if (m == rva) return k_prof_node_names[mid].name;
        if (m < rva) lo = mid + 1; else hi = mid - 1;
    }
    return "";
}

// ---- NODE CUT (census experiment): skip selected nodes at dispatch ----------
// Rules match work-fn RVA (+ optional SceneDrv rtId) and view side. Armed live
// from the overlay; every change is logged. Master switch default OFF. Used to
// census which nodes are droppable per view (CPU win) without quality loss.
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_NodeCutEnable = 0;
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_NodeCutRetVal = 1;  // dispatch return when skipping
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugNodeCutSkips = 0;
static std::atomic<uint32_t> g_cut_rva[64];    // 0 = empty slot
static std::atomic<uint32_t> g_cut_meta[64];   // (rtid<<8)|mode; rtid 0xFF=any; mode 0=off 1=vrcam 2=both 3=main
static std::atomic<int>      g_cut_active{0};
static bool node_cut_match(uint32_t rva, uint8_t rtid, bool vrcam) {
    if (g_cut_active.load(std::memory_order_relaxed) <= 0) return false;
    for (int i = 0; i < 64; ++i) {
        if (g_cut_rva[i].load(std::memory_order_relaxed) != rva) continue;
        const uint32_t meta = g_cut_meta[i].load(std::memory_order_relaxed);
        const uint32_t mode = meta & 0xFF, mrt = meta >> 8;
        if (!mode) continue;
        if (mrt != 0xFF && mrt != rtid) continue;
        if (mode == 2 || (mode == 1 && vrcam) || (mode == 3 && !vrcam)) return true;
    }
    return false;
}
extern "C" __declspec(dllexport) int CyberpunkVR_NodeCutGet(uint32_t rva, uint32_t rtid) {
    for (int i = 0; i < 64; ++i) {
        if (g_cut_rva[i].load(std::memory_order_relaxed) != rva) continue;
        const uint32_t meta = g_cut_meta[i].load(std::memory_order_relaxed);
        if ((meta >> 8) == rtid) return (int)(meta & 0xFF);
    }
    return 0;
}
extern "C" __declspec(dllexport) void CyberpunkVR_NodeCutSet(uint32_t rva, uint32_t rtid, int mode) {
    if (!rva) return;
    int slot = -1;
    for (int i = 0; i < 64; ++i) {
        if (g_cut_rva[i].load(std::memory_order_relaxed) == rva &&
            (g_cut_meta[i].load(std::memory_order_relaxed) >> 8) == rtid) { slot = i; break; }
    }
    if (slot < 0) {
        if (!mode) return;
        for (int i = 0; i < 64; ++i) {
            uint32_t expected = 0;
            if (g_cut_rva[i].compare_exchange_strong(expected, rva)) {
                g_cut_meta[i].store((rtid << 8), std::memory_order_relaxed);
                slot = i; break;
            }
        }
        if (slot < 0) { log("[nodecut] table full"); return; }
    }
    g_cut_meta[slot].store((rtid << 8) | (uint32_t)(mode & 0xFF), std::memory_order_release);
    int active = 0;
    for (int i = 0; i < 64; ++i)
        if (g_cut_rva[i].load(std::memory_order_relaxed) &&
            (g_cut_meta[i].load(std::memory_order_relaxed) & 0xFF)) ++active;
    g_cut_active.store(active, std::memory_order_release);
    log("[nodecut] rva=+0x%X rtid=%u mode=%d (%s) active=%d",
        rva, rtid, mode,
        mode == 1 ? "cut-vrcam" : mode == 2 ? "cut-both" : mode == 3 ? "cut-main" : "off",
        active);
}

// ---- live audit snapshots for the overlay (ms/frame since last dump-reset) --
extern "C" __declspec(dllexport) int CyberpunkVR_ProfSnapshotNodes(
        uint32_t* out_rva, double* out_msv, double* out_msm,
        uint32_t* out_cv, uint32_t* out_cm, int maxn) {
    // SELF ms per VIEW-FRAME. Self, because inclusive double-counts (SceneDrv contains the
    // scene passes it dispatches); per view-frame, because Present count is inflated by
    // frame generation while top-level dispatches are exactly one per rendered view.
    const uint64_t tm_n = g_prof_top_main.load(std::memory_order_relaxed);
    const uint64_t tv_n = g_prof_top_vrcam.load(std::memory_order_relaxed);
    const double fmain  = (double)(tm_n ? tm_n : 1);
    const double fvrcam = (double)(tv_n ? tv_n : 1);
    struct Row { uint32_t rva; double mv, mm; uint32_t cv, cm; };
    static Row rows[512];               // overlay/present thread only
    int n = 0;
    for (int i = 0; i < 512; ++i) {
        const uintptr_t w = g_prof_nodes[i].rva.load(std::memory_order_relaxed);
        if (!w) continue;
        Row r;
        r.rva = (uint32_t)w;
        r.mv = (double)g_prof_nodes[i].self_vrcam.load(std::memory_order_relaxed) * g_qpc_to_ms / fvrcam;
        r.mm = (double)g_prof_nodes[i].self_main.load(std::memory_order_relaxed) * g_qpc_to_ms / fmain;
        r.cv = g_prof_nodes[i].calls_vrcam.load(std::memory_order_relaxed);
        r.cm = g_prof_nodes[i].calls_main.load(std::memory_order_relaxed);
        if (r.mv + r.mm > 0.0) rows[n++] = r;
    }
    if (maxn > n) maxn = n;
    for (int i = 0; i < maxn; ++i) {
        int best = i;
        for (int j = i + 1; j < n; ++j)
            if (rows[j].mv + rows[j].mm > rows[best].mv + rows[best].mm) best = j;
        if (best != i) { Row t = rows[i]; rows[i] = rows[best]; rows[best] = t; }
        out_rva[i] = rows[i].rva; out_msv[i] = rows[i].mv; out_msm[i] = rows[i].mm;
        out_cv[i] = rows[i].cv;   out_cm[i] = rows[i].cm;
    }
    return maxn;
}
extern "C" __declspec(dllexport) int CyberpunkVR_ProfSnapshotPasses(
        uint32_t* out_rtid, double* out_msv, double* out_msm, int maxn) {
    uint64_t fr = g_prof_frames.load(std::memory_order_relaxed);
    const double frames = (double)(fr ? fr : 1);
    struct Row { uint32_t rt; double mv, mm; };
    static Row rows[128];
    int n = 0;
    for (int i = 0; i < 128; ++i) {
        const double mv = (double)g_prof_scenepass[i].ticks_vrcam.load(std::memory_order_relaxed) * g_qpc_to_ms / frames;
        const double mm = (double)g_prof_scenepass[i].ticks_main.load(std::memory_order_relaxed) * g_qpc_to_ms / frames;
        if (mv + mm <= 0.0) continue;
        rows[n].rt = (uint32_t)i; rows[n].mv = mv; rows[n].mm = mm; ++n;
    }
    if (maxn > n) maxn = n;
    for (int i = 0; i < maxn; ++i) {
        int best = i;
        for (int j = i + 1; j < n; ++j)
            if (rows[j].mv + rows[j].mm > rows[best].mv + rows[best].mm) best = j;
        if (best != i) { Row t = rows[i]; rows[i] = rows[best]; rows[best] = t; }
        out_rtid[i] = rows[i].rt; out_msv[i] = rows[i].mv; out_msm[i] = rows[i].mm;
    }
    return maxn;
}

// Dump ALL nodes + SceneDrv rtId breakdown (per-frame ms since last dump), reset.
// Emitted pipe-delimited so tools/node_audit_md.py can turn it into a named markdown
// audit; every reuse/experiment toggle is logged alongside, because a capture taken with
// an experiment armed silently invalidates the whole table (that is exactly what made the
// previous audit unusable).
static void prof_log_config();          // defined near the end: needs all the flag decls
extern "C" __declspec(dllexport) void CyberpunkVR_ProfDumpNodes() {
    struct Row { uintptr_t rva; int64_t tm, tv, sm, sv; uint32_t cm, cv, om, ov; };
    static Row rows[512];               // static: keep the hot path stack small
    int nrows = 0;
    for (int i = 0; i < 512; ++i) {
        const uintptr_t w = g_prof_nodes[i].rva.load(std::memory_order_relaxed);
        if (!w) continue;
        Row r;
        r.rva = w;
        r.tm = g_prof_nodes[i].ticks_main.exchange(0, std::memory_order_relaxed);
        r.tv = g_prof_nodes[i].ticks_vrcam.exchange(0, std::memory_order_relaxed);
        r.sm = g_prof_nodes[i].self_main.exchange(0, std::memory_order_relaxed);
        r.sv = g_prof_nodes[i].self_vrcam.exchange(0, std::memory_order_relaxed);
        r.cm = g_prof_nodes[i].calls_main.exchange(0, std::memory_order_relaxed);
        r.cv = g_prof_nodes[i].calls_vrcam.exchange(0, std::memory_order_relaxed);
        r.om = g_prof_nodes[i].ord_main.load(std::memory_order_relaxed);
        r.ov = g_prof_nodes[i].ord_vrcam.load(std::memory_order_relaxed);
        if (r.cm | r.cv) rows[nrows++] = r;
    }
    // sort by SELF time desc: inclusive double-counts parents (SceneDrv contains every
    // scene pass), so self is the only column you may rank or sum.
    for (int i = 0; i < nrows; ++i) {
        int best = i;
        for (int j = i + 1; j < nrows; ++j)
            if (rows[j].sm + rows[j].sv > rows[best].sm + rows[best].sv) best = j;
        if (best != i) { Row t = rows[i]; rows[i] = rows[best]; rows[best] = t; }
    }
    const uint64_t frames    = g_prof_frames.exchange(0, std::memory_order_relaxed);
    const uint64_t top_main  = g_prof_top_main.exchange(0, std::memory_order_relaxed);
    const uint64_t top_vrcam = g_prof_top_vrcam.exchange(0, std::memory_order_relaxed);
    const int64_t  now       = prof_now();
    const int64_t  t0        = g_prof_window_t0.exchange(now, std::memory_order_relaxed);
    const double   win_ms    = t0 ? (double)(now - t0) * g_qpc_to_ms : 0.0;
    prof_log_config();
    // ALL numbers below are WINDOW TOTALS, not per-frame. Divide main columns by
    // view_frames_main and vrcam columns by view_frames_vrcam (tools/node_audit_md.py).
    // NOT frame counts: the graph runner calls the executor directly for most nodes, so
    // almost every node is a top-level dispatch. The parser derives frames from the modal
    // per-node call count instead (tools/node_audit_md.py).
    log("[prof] AUDIT|nodes=%d|presents=%llu|node_dispatches_main=%llu|node_dispatches_vrcam=%llu"
        "|window_ms=%.1f|totals_not_per_frame",
        nrows, (unsigned long long)frames,
        (unsigned long long)top_main, (unsigned long long)top_vrcam, win_ms);
    log("[prof] HDR|ord_main|ord_vrcam|rva|name|calls_main|calls_vrcam"
        "|self_main_ms|self_vrcam_ms|incl_main_ms|incl_vrcam_ms");
    for (int i = 0; i < nrows; ++i) {
        const char* nm = CyberpunkVR_ProfNodeName((uint32_t)rows[i].rva);
        log("[prof] N|%u|%u|0x%06llX|%s|%u|%u|%.4f|%.4f|%.4f|%.4f",
            rows[i].om, rows[i].ov, (unsigned long long)rows[i].rva, nm[0] ? nm : "?",
            rows[i].cm, rows[i].cv,
            (double)rows[i].sm * g_qpc_to_ms, (double)rows[i].sv * g_qpc_to_ms,
            (double)rows[i].tm * g_qpc_to_ms, (double)rows[i].tv * g_qpc_to_ms);
    }
    // Scene passes, same convention: window totals. These are INCLUSIVE of the child nodes
    // the pass dispatches (which now have their own N rows), so pass ms and node ms overlap.
    log("[prof] HDRP|rtid|calls_main|calls_vrcam|incl_main_ms|incl_vrcam_ms");
    for (int i = 0; i < 256; ++i) {
        const int64_t tm = g_prof_scenepass[i].ticks_main.exchange(0, std::memory_order_relaxed);
        const int64_t tv = g_prof_scenepass[i].ticks_vrcam.exchange(0, std::memory_order_relaxed);
        const uint32_t cm = g_prof_scenepass[i].calls_main.exchange(0, std::memory_order_relaxed);
        const uint32_t cv = g_prof_scenepass[i].calls_vrcam.exchange(0, std::memory_order_relaxed);
        if (!(cm | cv)) continue;
        log("[prof] P|%d|%u|%u|%.4f|%.4f",
            i, cm, cv, (double)tm * g_qpc_to_ms, (double)tv * g_qpc_to_ms);
    }
    // (rtId, node) pairs: the pass CONTENT, which is what makes passes comparable between the
    // views given that rtId numbering is build-order and shifts between them.
    if (CyberpunkVR_DebugProfPairOverflow)
        log("[prof] PAIRWARN|overflow=%llu|rows_were_dropped_raise_g_prof_pairs",
            (unsigned long long)CyberpunkVR_DebugProfPairOverflow);
    CyberpunkVR_DebugProfPairOverflow = 0;
    log("[prof] HDRR|rtid|rva|name|calls_main|calls_vrcam|nested_main|nested_vrcam"
        "|owner_main|owner_vrcam|self_main_ms|self_vrcam_ms");
    for (int i = 0; i < 2048; ++i) {
        const uint64_t k = g_prof_pairs[i].key.exchange(0, std::memory_order_relaxed);
        const uint32_t cm = g_prof_pairs[i].calls_main.exchange(0, std::memory_order_relaxed);
        const uint32_t cv = g_prof_pairs[i].calls_vrcam.exchange(0, std::memory_order_relaxed);
        const uint32_t nm = g_prof_pairs[i].nested_main.exchange(0, std::memory_order_relaxed);
        const uint32_t nv = g_prof_pairs[i].nested_vrcam.exchange(0, std::memory_order_relaxed);
        const uint32_t om = g_prof_pairs[i].owner_main.exchange(0, std::memory_order_relaxed);
        const uint32_t ov = g_prof_pairs[i].owner_vrcam.exchange(0, std::memory_order_relaxed);
        const int64_t sm = g_prof_pairs[i].self_main.exchange(0, std::memory_order_relaxed);
        const int64_t sv = g_prof_pairs[i].self_vrcam.exchange(0, std::memory_order_relaxed);
        if (!k || !(cm | cv)) continue;
        const uint32_t rva = (uint32_t)(k & 0xFFFFFFFFull);
        const unsigned rtid = (unsigned)(k >> 32);
        const char* nm2 = CyberpunkVR_ProfNodeName(rva);
        log("[prof] R|%u|0x%06X|%s|%u|%u|%u|%u|%u|%u|%.4f|%.4f",
            rtid, rva, nm2[0] ? nm2 : "?", cm, cv, nm, nv, om, ov,
            (double)sm * g_qpc_to_ms, (double)sv * g_qpc_to_ms);
    }
}

// Answer the per-view capability question for the HUD node, and for nothing else.
//
// Ordered so the common case costs two loads: every other node on every other view goes straight
// through to the original. We only look further when the second eye is the one asking, and we
// never turn a YES into a NO -- only the specific NO that keeps the HUD out of the second eye.
// ---- the per-view RenderMask ---------------------------------------------------------------
//
// sub_14021BE28(wc, desc) is neither a private HUD gate nor an opaque "capability": every
// descriptor it is ever handed is a NAMED render-mask entry, registered at startup by a
// one-line function. The registrar for the one that matters here is
//     sub_1400F76B0:  "Rendering/RenderMask/DistantLights"  ->  word_143487D70
// and the test passes when the view's 32-qword mask at view+0x18A0 is a superset of the
// descriptor's words. NOTE THE +8: it compares mask[i] against desc[i+1], so the required
// words begin at descriptor+8. Reading them from descriptor+0 -- which the first grant did --
// ORs the wrong words in and the test still fails. That, not the engine, is why "granting the
// capability changed nothing".
//
// Mapping the measured refusals onto the name table settles the unlit street lamps:
//     77CED4 ClusteredLightsCull + 77D308 RenderLightBuffers  ->  RenderMask/DistantLights
//     77D214 AutoSpawnOnTerrain  + 153844 RenderShadowCascade ->  RenderMask/AutoGrass
//     1EE760 DrawHUD                                          ->  RenderMask/HUD
// and the Nsight capture agrees end to end: with DistantLights present MAIN clears the 20-byte
// argument buffer Resource_1359, fills it with a 1964-group PipelineState_597 dispatch, binds a
// 36-index unit cube and issues CommandSignature_81 -> DrawIndexedInstanced(36, 1821), i.e.
// 1821 local-light proxy volumes into the lighting target. The RTT view is refused all of it,
// so its lamps light nothing while their emissive surface still renders -- exactly the report.
//
// Only real render-mask categories belong in this table. Half of the refusal list is
// `Rendering/Debug/...` -- distant-shadow previews, chrome balls, probe overlays -- and turning
// those on for the second eye is not a fix, it is a debug overlay. The earlier blanket grant
// (CapGrant 2) did turn them on.
struct RenderMaskEntry { const char* name; uint32_t desc_rva; };
static const RenderMaskEntry kRenderMasks[] = {
    { "DistantLights",       0x3487D70 },   // bit 0 -- the unlit lamps           [on by default]
    { "AutoGrass",           0x34880A0 },   // bit 1 -- terrain scatter + its cascade
    { "Foliage",             0x3487F90 },   // bit 2
    { "Decals",              0x3487B50 },   // bit 3
    { "Terrain",             0x3487E80 },   // bit 4
    { "EnvProbes",           0x34881B0 },   // bit 5
    { "Emissive",            0x34882C0 },   // bit 6
    { "LightChannels",       0x34883D0 },   // bit 7
    { "Fog",                 0x34884E0 },   // bit 8
    { "Lights",              0x3487C60 },   // bit 9
    { "ClearLighting",       0x3488700 },   // bit 10
    { "GameplayPostProcess", 0x3487930 },   // bit 11 -- scanner/focus tint etc  [on by default]
    { "HUD",                 0x3487820 },   // bit 12 -- we composite the HUD ourselves: leave off
};
static const uint32_t kRenderMaskCount =
    static_cast<uint32_t>(sizeof(kRenderMasks) / sizeof(kRenderMasks[0]));

// THE VIEW BITSET IS NOT ONE CONTIGUOUS BITSET -- corrected 2026-07-31 by static reverse.
// The live diff named one clean MAIN-set/VRCAM-zero run in the graph context, 1870-1873{1},
// and the old note here read that as feature bit (0x1870-0x17D0)/8*64 = 1280. It is not.
// Every one of the 993 call sites of the feature test sub_14023AF5C passes a bit in 0..91 --
// nothing reads 1280, and granting it did nothing on screen, as it could not. So 0x17D0 holds
// f0/f1 and the region up to the mask at 0x18A0 is other per-view state, not more bits.
// view+0x1870 remains an unexplained MAIN-only dword; identify its reader before writing it.


// One bit per row above.
//
// bit 0  DistantLights       -- local lamps cast no light in the second eye without it.
// bit 11 GameplayPostProcess -- the scanner's green screen tint, and Sandevistan / Kerenzikov /
//        focus mode / cyberspace with it. CRenderNode_GameplayPostFX (0x77120C) is nothing but
//        this gate: `if (sub_14021BE28(wc, word_143487930)) do_the_work();`. The node audit had
//        already measured the shape of it -- MAIN 0.0134 ms vs VRCAM 0.0005 ms, i.e. dispatched
//        and instantly bailed, the same signature ScreenSpaceRain had before the wetness fix.
//
// AutoGrass (bit 1) is the remaining real difference and is left OFF: it makes the second view
// re-issue the terrain-scatter batches in the G-buffer and in both shadow cascades, which is a
// per-frame cost nobody has asked for yet.
// bit 1 AutoGrass added 2026-07-30: the second eye had bushes but no grass. It was left off
// deliberately -- it makes the view re-issue the terrain-scatter batches in the G-buffer AND
// in both shadow cascades -- on the grounds that nobody had reported it missing. Someone has.
//
// AND IT IS NOT THE SHADOW FLICKER, measured 2026-07-31. With the grant taken back to the
// point where the second view was refused the terrain scatter outright -- census read
// PrepareAutoSpawn V=0/5337, i.e. no grass in that eye at all -- the shadows still flickered.
// Do not re-suspect this bit. The symptom is two shadow SETS alternating, it is specific to
// where the head is standing and pointing, and grass is not in it.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_RenderMaskGrant =
    (1u << 0) | (1u << 1) | (1u << 11);
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugRenderMaskGrants = 0;
static std::atomic<uintptr_t> g_vrcam_ctx_seen{0};
static void render_mask_report();     // defined further down, where g_main_ctx exists

// The mask lives on the view object, which outlives the frame, so in practice this is one-shot
// per view: after the first pass nothing is missing and the loop finds no work. Re-checking on
// every node dispatch is what keeps it correct across a view being recreated.
static void render_mask_grant(uintptr_t ctx) {
    const uint32_t want = CyberpunkVR_RenderMaskGrant;
    if (!ctx || !want || !g_exe_base) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(g_exe_base);
    __try {
        uint64_t* have = reinterpret_cast<uint64_t*>(ctx + 6304);
        for (uint32_t k = 0; k < kRenderMaskCount; ++k) {
            if (!(want & (1u << k))) continue;
            const uint64_t* need =
                reinterpret_cast<const uint64_t*>(base + kRenderMasks[k].desc_rva) + 1;
            bool changed = false;
            for (int i = 0; i < 32; ++i) {
                const uint64_t missing = need[i] & ~have[i];
                if (missing) { have[i] |= missing; changed = true; }
            }
            if (changed)
                InterlockedIncrement64(
                    reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugRenderMaskGrants));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    g_vrcam_ctx_seen.store(ctx, std::memory_order_release);
}

// Capability-refusal census. sub_14021BE28 is not the HUD's private gate: ClusteredLightsCull
// runs its whole body only `if (... || sub_14021BE28(a2, &word_143487D70) != 0)`, and other
// nodes use it with their own descriptors. DrawHUD's refusal was found by hand and cost a
// session; enumerating every (node, descriptor) the engine denies the second view is the same
// work done once, for all of them.
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_CapCensus = 1;   // OFF: [cap] per-view gate refusals -- the first thing to switch on for a new "the second eye is missing X"
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCapDenies = 0;
struct CapDeny { uint32_t node_rva, desc_rva; uint64_t hits; };
static std::array<CapDeny, 32> g_cap_deny{};
static uint32_t g_cap_deny_n = 0;
static std::mutex g_cap_deny_mtx;

static void cap_census_note(uintptr_t work, uintptr_t required) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(g_exe_base);
    if (!base) return;
    const uint32_t nrva = (work > base) ? static_cast<uint32_t>(work - base) : 0;
    const uint32_t drva = (required > base) ? static_cast<uint32_t>(required - base) : 0;
    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugCapDenies));
    bool dump = false;
    {
        std::lock_guard<std::mutex> lk(g_cap_deny_mtx);
        uint32_t i = 0;
        for (; i < g_cap_deny_n; ++i)
            if (g_cap_deny[i].node_rva == nrva && g_cap_deny[i].desc_rva == drva) break;
        if (i == g_cap_deny_n) {
            if (g_cap_deny_n >= g_cap_deny.size()) return;
            g_cap_deny[g_cap_deny_n++] = { nrva, drva, 0 };
            dump = true;                       // a pair we have not seen -> report the table
        }
        ++g_cap_deny[i].hits;
        if (dump) {
            char line[900];
            int used = 0;
            line[0] = '\0';
            for (uint32_t k = 0; k < g_cap_deny_n; ++k)
                if (used < static_cast<int>(sizeof(line)) - 32)
                    used += snprintf(line + used, sizeof(line) - used, "node %X/desc %X ",
                                     g_cap_deny[k].node_rva, g_cap_deny[k].desc_rva);
            log("[cap] VRCAM capability refusals (%u distinct): %s", g_cap_deny_n, line);
        }
    }
}

// Granting = making the test pass HONESTLY, by OR-ing the bits the descriptor asks for into the
// view's own 32-qword mask at ctx+6304 -- exactly what fixed the HUD. Not the same thing as
// returning 1 from the gate: that leaves the mask short, so the next node to ask gets refused
// again and anything downstream that reads the mask still sees a crippled view.
//
// The measured refusals, by node:
//   77CED4 ClusteredLightsCull, 77D308 RenderLightBuffers   <- the reported unlit lights
//   77E610 ReflectionProbes, 786BCC RenderShadowmask, 153844 RenderShadowCascade,
//   6212EC DecoupledParticleLighting, 775ACC SetRenderTargetsMain, 774CF8 HistogramUpdate,
//   77120C GameplayPostFX, 77B638/77D214 AutoSpawnOnTerrain
// 0 = census only, 1 = the two light nodes (default: the smallest change that addresses the
// symptom), 2 = every refusal.
// SUPERSEDED by CyberpunkVR_RenderMaskGrant, and left at 0.
// Two things were wrong with granting here. It read the required words from descriptor+0 when
// the test compares against descriptor+8, so it never actually satisfied anything -- and it
// fired on EVERY refusal, which includes a dozen `Rendering/Debug/...` overlays the engine is
// right to withhold. The named table above grants one specific category from the engine's own
// descriptor. Kept only so the census can still be run with the grant off.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_CapGrant = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCapGrants = 0;
constexpr uint32_t CLUSTERED_LIGHTS_CULL_RVA = 0x77CED4;
constexpr uint32_t RENDER_LIGHT_BUFFERS_RVA  = 0x77D308;

static bool cap_grant_required(uintptr_t work_context, uintptr_t required) {
    if (!work_context || !required) return false;
    __try {
        const uintptr_t ctx = *reinterpret_cast<uintptr_t*>(work_context + 0x18);
        if (!ctx) return false;
        uint64_t* have = reinterpret_cast<uint64_t*>(ctx + 6304);
        // +1 qword: sub_14021BE28 compares mask[i] against descriptor[i+1].
        const uint64_t* need = reinterpret_cast<const uint64_t*>(required) + 1;
        bool changed = false;
        for (int i = 0; i < 32; ++i) {
            const uint64_t missing = need[i] & ~have[i];
            if (missing) { have[i] |= missing; changed = true; }
        }
        return changed;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static uint8_t __fastcall Detour_ViewFeatureCheck(uintptr_t work_context, uintptr_t required) {
    const uint8_t r0 = g_view_feature_check_orig(work_context, required);
    if (!r0 && t_vrcam_node_active && CyberpunkVR_CapCensus)
        cap_census_note(t_current_node_work, required);
    if (!r0 && t_vrcam_node_active && CyberpunkVR_CapGrant && g_exe_base) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(g_exe_base);
        const uint32_t nrva = (t_current_node_work > base)
                                  ? static_cast<uint32_t>(t_current_node_work - base) : 0;
        const bool wanted = (CyberpunkVR_CapGrant >= 2) ||
                            nrva == CLUSTERED_LIGHTS_CULL_RVA ||
                            nrva == RENDER_LIGHT_BUFFERS_RVA;
        if (wanted && cap_grant_required(work_context, required)) {
            InterlockedIncrement64(
                reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugCapGrants));
            return g_view_feature_check_orig(work_context, required);   // now it passes on merit
        }
    }
    if (!CyberpunkVR_HudInVrcam || !t_vrcam_node_active) return r0;
    const uint8_t r = r0;
    if (r) return r;
    const uintptr_t work = t_current_node_work;
    if (!work || !g_exe_base || work <= reinterpret_cast<uintptr_t>(g_exe_base)) return r;
    if (static_cast<uint32_t>(work - reinterpret_cast<uintptr_t>(g_exe_base)) != DRAWHUD_WORK_RVA) {
        return r;
    }
    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugHudGateDenied));
    // DrawHUD asks twice (word_143487820 in the prologue, word_143487930 further in), so this
    // deliberately does not discriminate by descriptor -- inside the HUD node, on the second
    // eye, every capability refusal is the same refusal.
    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugHudGateForced));
    static bool s_said = false;
    if (!s_said) {
        s_said = true;
        log("[hud] second-eye capability refusal overridden inside DrawHUD -- HUD should now "
            "render for VRCAM (set CyberpunkVR_HudInVrcam=0 to revert)");
    }
    return 1;
}

// Defined with the cloud block further down; called from here because the node dispatch is the
// EARLIEST point in a view's frame, and a hole has to be filled before its consumer runs. The
// cloud hook was too late: the wetness bytes landed (the diff stopped reporting the hole) but
// ScreenSpaceRain had already read zero and bailed.
static void viewdata_fill_from_wc(void* wc);
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_ViewDataFixMask;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_FogMirrorMask;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_EnvMirrorMask;

// ---- per-view node census ---------------------------------------------------------------
// Which passes does the engine actually dispatch for each view? "VRCAM is missing effect X"
// is otherwise answered by breakpointing candidate nodes one at a time, which is slow and only
// ever tests the guesses one happened to make. This records the SET of work-fn RVAs each view
// runs and logs the difference, so the answer arrives in one pass over a live frame.
// (The two arrays have existed as exports for a long time but nothing ever filled them, which
// is why they always read back zero.)
extern "C" __declspec(dllexport) int32_t CyberpunkVR_NodeCensus = 1;
static std::mutex g_census_mtx;

static void node_census_add(uint32_t rva, bool vrcam) {
    uintptr_t* list = vrcam ? CyberpunkVR_DebugSecondaryNodeWorks
                            : CyberpunkVR_DebugMainNodeWorks;
    uint32_t& n     = vrcam ? CyberpunkVR_DebugSecondaryNodeUnique
                            : CyberpunkVR_DebugMainNodeUnique;
    std::lock_guard<std::mutex> lk(g_census_mtx);
    for (uint32_t i = 0; i < n; ++i)
        if (static_cast<uint32_t>(list[i]) == rva) return;
    if (n >= 256) return;
    list[n] = rva;
    ++n;
}

// The interesting output is the asymmetry, so log exactly that: RVAs one view dispatches and
// the other does not. Time-gated and one-shot-ish -- the sets converge within a second or two,
// after which the same two lines just repeat.
static void node_census_dump() {
    static uint64_t s_last = 0;
    const uint64_t now = GetTickCount64();
    if (s_last && now - s_last < 20000) return;
    uint32_t mainRva[256], vrRva[256], mainN, vrN;
    {
        std::lock_guard<std::mutex> lk(g_census_mtx);
        mainN = CyberpunkVR_DebugMainNodeUnique;
        vrN   = CyberpunkVR_DebugSecondaryNodeUnique;
        for (uint32_t i = 0; i < mainN; ++i)
            mainRva[i] = static_cast<uint32_t>(CyberpunkVR_DebugMainNodeWorks[i]);
        for (uint32_t i = 0; i < vrN; ++i)
            vrRva[i] = static_cast<uint32_t>(CyberpunkVR_DebugSecondaryNodeWorks[i]);
    }
    if (!mainN || !vrN) return;      // wait until both views have been seen at all
    s_last = now;
    for (int pass = 0; pass < 2; ++pass) {
        const uint32_t* a = pass ? vrRva : mainRva;
        const uint32_t* b = pass ? mainRva : vrRva;
        const uint32_t  na = pass ? vrN : mainN, nb = pass ? mainN : vrN;
        char line[1024];
        int  used = 0, count = 0;
        line[0] = '\0';
        for (uint32_t i = 0; i < na; ++i) {
            bool shared = false;
            for (uint32_t j = 0; j < nb && !shared; ++j) shared = (b[j] == a[i]);
            if (shared) continue;
            ++count;
            if (used < (int)sizeof(line) - 16)
                used += snprintf(line + used, sizeof(line) - used, "%X ", a[i]);
        }
        log("[census] %s-only nodes: %d of %u | %s", pass ? "VRCAM" : "MAIN",
            count, na, count ? line : "(none)");
    }
}

// ---- the sky pass, as reversed -----------------------------------------------------------
//
// CRenderNode_RenderSkyScattering (sub_1407818B0) only checks feature 35 and forwards to
// sub_1407818F8, and THAT is where the decision is:
//
//     v3 = *(QWORD*)(*(QWORD*)(a2+32) + 96);          // the sky manager
//     v4 = *(DWORD*)(sub_1401ED930(a2) + 3988);       // viewData+0xF94, the AA/upscaler mode
//     v8 = 32 * *(BYTE*)(view + 5856);                // 32-byte record, INDEXED BY THE VIEW
//     if ( *(DWORD*)(v8 + v3 + 72) && !v4 || v7 ) { ...build the sky... }
//       ...
//     *(BYTE*)(v8+v3+80) = slot+1;      // one of six slots per frame
//     if (v7 || *(BYTE*)(v8+v3+80) >= 6) { ...publish, timestamp, reset the counter... }
//
// Two things follow, and both are measurable rather than arguable.
//
// The sky LUT is built AMORTISED -- six slots, one per frame, then published and the counter
// reset with an InterlockedExchange. That is shared mutable state with a work counter, the same
// shape as the GI clipmaps and the shadow cascades, and the same shape that has produced every
// "two versions alternating" symptom in this project. Whether the two views share it depends
// entirely on the byte at view+5856 (0x16E0): different index, private record; same index, they
// take turns filling one sky.
//
// And the gate reads viewData+0xF94 -- the very field StreamlineHistoryFix WRITES, mirroring
// MAIN's AA mode onto VRCAM. So our own fix is an input to the sky decision, which nobody knew.
//
// Report both, for both views. Reading only; nothing here changes engine state.
extern "C" __declspec(dllexport) int32_t CyberpunkVR_SkyProbe = 1;   // answered: both views index sky record 0, AA mode 0
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugSkyIdxMain  = 0xFFFF;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugSkyIdxVrcam = 0xFFFF;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugMainAaMode;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugVrcamAaMode;

// ---- the cloud lighting shader selector -----------------------------------------------------
//
// sub_14061BE74 RenderVolumetricCloudsLighting, decompiled:
//     v5  = *(int**)(ctx + 7592);        // ctx+0x1DA8, the per-view cloud state
//     v20 = v5[8] == 1;                  // an int at cloudState+32
//     if (v20) { shader -661749514 } else { shader -298149306 }
// One int chooses between two different lighting shaders. If the views disagree on it they are
// lighting their clouds with different permutations, which is exactly a brightness difference --
// and it fits the audit, where this node costs the second view 2.2x what it costs MAIN, and the
// clouds node 1.7x. More time means the heavier branch.
//
// Everything upstream is already equal by measurement: viewData has no atmosphere field left
// differing, the cloud CB's only mirrorable field (0x40) is mirrored, and the pass's other inputs
// viewData+0x430 / +0x550 sit inside mirrored ranges. This selector is what is left.
//
// Probe first, write second. CloudLightMirror defaults OFF: swapping a shader permutation is not
// the same class of change as copying a float, and the measurement costs nothing.
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_CloudLightProbe  = 1;   // answered: selector 2 on both; the state object is pooled per frame and not diffable across views
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_CloudLightMirror = 0;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugCloudSelMain  = 0xFFFFFFFF;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugCloudSelVrcam = 0xFFFFFFFF;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCloudSelWrites = 0;

// RULED OUT: the selector reads 2 on both views, so they light the clouds with the same shader
// (-298149306, the else branch). Kept as a live probe because it cost nothing and the answer is
// worth keeping visible.
//
// That leaves the cloud state object itself. ctx+0x1DA8 is the one structure in this whole chase
// that has never been diffed -- viewData, the graph context and the cloud constant buffer all
// have been, and each of the three named its own defect. Same instrument, last structure. The
// known fields are +24/+28 (resources), +32 (the selector) and +40 (read by the clouds node), so
// 256 bytes with the chunked copy covers it without assuming a size.
static uint8_t g_cloudst_main[256];
static std::atomic<size_t> g_cloudst_len{0};
static std::mutex g_cloudst_mtx;

// SEH cannot share a frame with anything that unwinds (C2712), so the guarded reads live here.
static size_t cloudst_read(void* dst, uintptr_t src, size_t n) {
    size_t done = 0;
    while (done < n) {
        const size_t step = (n - done) < 32 ? (n - done) : 32;
        __try { memcpy(static_cast<uint8_t*>(dst) + done,
                       reinterpret_cast<const void*>(src + done), step); }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        done += step;
    }
    return done;
}
static uintptr_t cloudst_ptr(uintptr_t ctx) {
    __try { return *reinterpret_cast<uintptr_t*>(ctx + 7592); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}


// ---- the same snapshot, taken at the SAME point of the frame -------------------------------
//
// The dispatch-time version of this was worthless and said so: it snapped MAIN on whatever MAIN
// node ran and VRCAM on whatever VRCAM node ran, i.e. at two unrelated points of the frame. For
// fields the engine fills and clears as the frame proceeds that compares nothing -- which is why
// one capture showed the populated sub-block only on MAIN and the next showed it only on VRCAM,
// and why the counter at +0x18 went DOWN between captures (the object comes from a pool).
//
// Hooking the clouds node instead puts both snapshots at one place: the node's own entry. Then a
// field that still differs really differs.
constexpr uintptr_t CLOUDS_NODE_RVA = 0x61B5B4;   // CRenderNode_RenderVolumetricClouds
using CloudsNodeFn = char(__fastcall*)(void*, void*);
static CloudsNodeFn g_orig_clouds_node = nullptr;
static uint8_t g_cst_snap[2][256];
static size_t  g_cst_len[2] = { 0, 0 };
static std::mutex g_cst_mtx;

static void clouds_node_note(uintptr_t ctx, bool vrcam) {
    const uintptr_t st = cloudst_ptr(ctx);
    if (!st) return;
    uint8_t cur[256];
    const size_t got = cloudst_read(cur, st, sizeof(cur));
    if (got < 64) return;
    bool report = false;
    uint8_t ref[256];
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lk(g_cst_mtx);
        memcpy(g_cst_snap[vrcam ? 1 : 0], cur, got);
        g_cst_len[vrcam ? 1 : 0] = got;
        if (vrcam && g_cst_len[0]) {
            n = g_cst_len[0] < got ? g_cst_len[0] : got;
            n &= ~size_t(3);
            memcpy(ref, g_cst_snap[0], n);
            report = n >= 64;
        }
    }
    if (!report) return;
    static std::atomic<uint64_t> s_next{0};
    const uint64_t now = GetTickCount64();
    uint64_t due = s_next.load(std::memory_order_relaxed);
    if (now < due) return;
    if (!s_next.compare_exchange_strong(due, now + 6000, std::memory_order_relaxed)) return;
    const uint32_t* m = reinterpret_cast<const uint32_t*>(ref);
    const uint32_t* v = reinterpret_cast<const uint32_t*>(cur);
    char line[900];
    int used = 0;
    line[0] = 0;
    for (size_t k = 0; k < n / 4; ++k) {
        if (m[k] == v[k]) continue;
        if (used < static_cast<int>(sizeof(line)) - 32)
            used += snprintf(line + used, sizeof(line) - used, "%zX{%08X|%08X} ",
                             k * 4, m[k], v[k]);
    }
    log("[cloudnode] state at the clouds-node entry, %zu bytes, M|V: %s",
        n, used ? line : "(identical)");
}

static uintptr_t clouds_ctx_of(void* wc) {
    __try { return wc ? *reinterpret_cast<uintptr_t*>(
                            reinterpret_cast<uint8_t*>(wc) + 0x18) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static bool clouds_is_vrcam(uintptr_t ctx) {
    __try { return ctx && *reinterpret_cast<uint64_t*>(ctx + 0x28) == g_vrcam_ctx_key; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static char __fastcall Detour_CloudsNode(void* a1, void* a2) {
    if (CyberpunkVR_CloudLightProbe) {
        const uintptr_t ctx = clouds_ctx_of(a2);
        if (ctx) clouds_node_note(ctx, clouds_is_vrcam(ctx));
    }
    return g_orig_clouds_node(a1, a2);
}


static void cloud_sel_note(uintptr_t ctx, bool vrcam) {
    if (!CyberpunkVR_CloudLightProbe || !ctx) return;
    __try {
        const uintptr_t st = *reinterpret_cast<uintptr_t*>(ctx + 7592);
        if (!st) return;
        int32_t* sel = reinterpret_cast<int32_t*>(st + 32);
        if (vrcam) {
            CyberpunkVR_DebugCloudSelVrcam = static_cast<uint32_t>(*sel);
            if (CyberpunkVR_CloudLightMirror &&
                CyberpunkVR_DebugCloudSelMain != 0xFFFFFFFF &&
                *sel != static_cast<int32_t>(CyberpunkVR_DebugCloudSelMain)) {
                *sel = static_cast<int32_t>(CyberpunkVR_DebugCloudSelMain);
                InterlockedIncrement64(
                    reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugCloudSelWrites));
            }
        } else {
            CyberpunkVR_DebugCloudSelMain = static_cast<uint32_t>(*sel);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    static std::atomic<uint64_t> s_next{0};
    const uint64_t now = GetTickCount64();
    uint64_t due = s_next.load(std::memory_order_relaxed);
    if (now < due) return;
    if (!s_next.compare_exchange_strong(due, now + 5000, std::memory_order_relaxed)) return;
    log("[cloudsel] cloudState+32 (picks the lighting shader) -- MAIN %d  VRCAM %d  %s  "
        "mirror=%d writes=%llu",
        (int)CyberpunkVR_DebugCloudSelMain, (int)CyberpunkVR_DebugCloudSelVrcam,
        (CyberpunkVR_DebugCloudSelMain == CyberpunkVR_DebugCloudSelVrcam)
            ? "same shader" : "<- DIFFERENT SHADERS",
        CyberpunkVR_CloudLightMirror, CyberpunkVR_DebugCloudSelWrites);
}


static void sky_probe_note(uintptr_t ctx, bool vrcam) {
    if (!CyberpunkVR_SkyProbe || !ctx) return;
    __try {
        const uint32_t idx = *reinterpret_cast<uint8_t*>(ctx + 5856);
        if (vrcam) CyberpunkVR_DebugSkyIdxVrcam = idx;
        else       CyberpunkVR_DebugSkyIdxMain  = idx;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    static std::atomic<uint64_t> s_next{0};
    const uint64_t now = GetTickCount64();
    uint64_t due = s_next.load(std::memory_order_relaxed);
    if (now < due) return;
    if (!s_next.compare_exchange_strong(due, now + 5000, std::memory_order_relaxed)) return;
    log("[sky] sky-record index view+0x16E0 -- MAIN %u  VRCAM %u   %s     "
        "AA mode viewData+0xF94 -- MAIN %u  VRCAM %u",
        CyberpunkVR_DebugSkyIdxMain, CyberpunkVR_DebugSkyIdxVrcam,
        (CyberpunkVR_DebugSkyIdxMain == CyberpunkVR_DebugSkyIdxVrcam)
            ? "<- SAME RECORD: the two views share one amortised sky"
            : "<- separate records",
        CyberpunkVR_DebugMainAaMode, CyberpunkVR_DebugVrcamAaMode);
}

static uint8_t __fastcall Detour_NodeDispatch(
        uintptr_t* node, uint8_t* work_context, void* args) {
    // Attribution FIRST (work-fn, vrcam view, SceneDrv rtId): used by the profiler,
    // the NODE-CUT gate and the mirror path. The OM/barrier hooks read
    // t_current_node_work to gate the tonemap RT0 snapshot + mirror RTV capture.
    bool vrcam_node = false;
    bool node_owner_bit = false;  // node-arg +0x30 bit1: the engine's "this view owns the
                                  // shared update" flag (audit column, see prof_pair_add)
    uintptr_t prof_work = 0;
    uint8_t scene_rtid = 0xFF;    // pass/RT slot id from ctx+0x38
    uint64_t view_key = 0;        // ctx+0x28: 0 = MAIN, g_vrcam_ctx_key = VRCAM, else other
    bool view_key_known = false;  // false when this node carries no view ctx at all
    __try {
        const uintptr_t vtable = node ? *node : 0;
        prof_work = vtable ? *reinterpret_cast<uintptr_t*>(vtable + 8) : 0;
        if (work_context) {
            const uintptr_t ctx = *reinterpret_cast<uintptr_t*>(work_context + 0x18);
            if (ctx) {
                view_key = *reinterpret_cast<uint64_t*>(ctx + 0x28);
                view_key_known = true;
            }
            if (ctx && *reinterpret_cast<uint64_t*>(ctx + 0x28) == g_vrcam_ctx_key) {
                vrcam_node = true;   // g_vrcam_ctx_key
                // Belt and braces: fg_observe grants this at graph-build time, but only for the
                // builder path it sees. Re-asserting it here means no node of this view can run
                // before the capability is present, whichever of them collects the HUD.
                hud_grant_capability(ctx);
                // Named render-mask categories (DistantLights by default). Same mechanism as
                // the HUD grant above, but sourced from the engine's own descriptor rather
                // than from hand-picked bit numbers.
                render_mask_grant(ctx);
            }
            if (ctx) sky_probe_note(ctx, view_key == g_vrcam_ctx_key);
            if (ctx) cloud_sel_note(ctx, view_key == g_vrcam_ctx_key);
            // Always-on chain diagnostic. Zero here means the engine never dispatched a node
            // for a view whose key matches ours -- i.e. the component is not enabled, or its
            // virtualCameraName is not what we hashed. Non-zero here with a dead mirror moves
            // the search downstream (RTV capture / blit submit).
            if (vrcam_node) { ++CyberpunkVR_DebugVrcamNodeHits; render_mask_report(); }
            // MAIN identity, step 1. Note the deliberate absence of a `ctx` requirement:
            // these nodes run with work_context+0x18 == 0, so a ctx-keyed bind here can never
            // fire. The view OBJECT is what they carry, so that is what we record.
            if (!vrcam_node && prof_work && g_exe_base) {
                const uint32_t rva = (uint32_t)(prof_work - (uintptr_t)g_exe_base);
                if (rva == MAIN_PRESENT_WORK_RVA || rva == MAIN_STARTRENDER_WORK_RVA) {
                    const uintptr_t obj = sl_view_obj(work_context);
                    if (obj &&
                        g_main_view_obj.exchange(obj, std::memory_order_release) != obj)
                        ++CyberpunkVR_DebugMainObjBinds;
                }
            }
            node_owner_bit = (work_context[0x30] & 2) != 0;
            // rtId is ctx+0x38. There is NO valid-flag at +0x39: the engine's own executor
            // indexes its RT table with *(u8*)(a2+56) unconditionally (engine_re/dumps/
            // B_framegraph.md:7,45). The old `ctx[0x39]==1` guard was a bad inference and
            // dropped ~90% of the scene passes (4 of 41 SceneDrv calls/frame got attributed).
            scene_rtid = work_context[0x38];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { prof_work = 0; }
    // Save/restore, like t_vrcam_node_active below. Without the restore a node that dispatches
    // child nodes has this pointing at the LAST CHILD for the rest of its body, so every GPU
    // call it makes afterwards is attributed to the child. That is not academic: it is what put
    // ClusteredLightsCull's 1964-group dispatch under one node for MAIN and another for VRCAM,
    // making a shared dispatch look MAIN-exclusive in the census.
    const uintptr_t previous_node_work = t_current_node_work;
    t_current_node_work = prof_work;
    const uint32_t work_rva = (prof_work && g_exe_base &&
            prof_work > reinterpret_cast<uintptr_t>(g_exe_base))
        ? static_cast<uint32_t>(prof_work - reinterpret_cast<uintptr_t>(g_exe_base)) : 0;
    // Close VRCAM's viewData holes as early as the view is seen at all, so every consumer in
    // the frame reads the filled value rather than the zero the pool handed out.
    if ((CyberpunkVR_ViewDataFixMask || CyberpunkVR_FogMirrorMask ||
         CyberpunkVR_EnvMirrorMask) && vrcam_node && work_context)
        viewdata_fill_from_wc(work_context);
    // Per-view node census. Only views we can name: nodes dispatched with no view ctx are
    // global and cannot be attributed to either eye.
    if (CyberpunkVR_NodeCensus && work_rva && view_key_known) {
        if (view_key == 0) { node_census_add(work_rva, false); node_census_dump(); }
        else if (view_key == g_vrcam_ctx_key) node_census_add(work_rva, true);
    }
    // NODE CUT census: skip the whole node when an armed rule matches (see table above).
    if (CyberpunkVR_NodeCutEnable && work_rva &&
            node_cut_match(work_rva, scene_rtid, vrcam_node)) {
        InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(
            &CyberpunkVR_DebugNodeCutSkips));
        t_current_node_work = previous_node_work;
        return static_cast<uint8_t>(CyberpunkVR_NodeCutRetVal);
    }
    // Mirror: detect the vrcam CopyToTexture node and arm per-node capture state.
    const bool mirror_copy_node = is_vrcam_copy_to_texture(node, work_context);
    const bool previous_mirror_active = t_mirror_copy_node_active;
    ID3D12Resource* const previous_mirror_rtv = t_mirror_copy_rtv;
    const DXGI_FORMAT previous_mirror_format = t_mirror_copy_rtv_format;
    ID3D12GraphicsCommandList* const previous_mirror_list = t_mirror_copy_list;
    if (mirror_copy_node) {
        g_eye_node_hits.fetch_add(1, std::memory_order_relaxed);
        t_mirror_copy_node_active = true;
        t_mirror_copy_rtv = nullptr;
        t_mirror_copy_rtv_format = DXGI_FORMAT_UNKNOWN;
        t_mirror_copy_list = nullptr;
        t_mirror_src_state = (uint32_t)D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    // Does the HUD node reach the second view at all? This is the measurement that decides
    // whether the capability override can work: an override is useless on a node the engine
    // never dispatches for that view. Counted unconditionally -- two compares on a path that
    // already computed work_rva.
    if (work_rva == DRAWHUD_WORK_RVA) {
        if (vrcam_node) InterlockedIncrement64(
                            reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugHudNodeVrcam));
        else            InterlockedIncrement64(
                            reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugHudNodeMain));
        // The list of bit positions the second view lacks -- once per view, then never again.
        hud_dump_capability_mask(work_context, vrcam_node);
        // Readable without a debugger: one line every ~15 s, keyed off the MAIN count so it
        // cannot spin when the second view is absent.
        if ((CyberpunkVR_DebugHudNodeMain % 900) == 1) {
            log("[hud] DrawHUD main=%llu vrcam=%llu | gate denied=%llu forced=%llu | "
                "blocks null=%llu ok=%llu lent=%llu | capGrants=%llu w%u=%016llX "
                "| HudInVrcam=%d borrow=%d grant=%d",
                (unsigned long long)CyberpunkVR_DebugHudNodeMain,
                (unsigned long long)CyberpunkVR_DebugHudNodeVrcam,
                (unsigned long long)CyberpunkVR_DebugHudGateDenied,
                (unsigned long long)CyberpunkVR_DebugHudGateForced,
                (unsigned long long)CyberpunkVR_DebugHudBlockNull,
                (unsigned long long)CyberpunkVR_DebugHudBlockOk,
                (unsigned long long)CyberpunkVR_DebugHudBlockLent,
                (unsigned long long)CyberpunkVR_DebugHudCapGrants,
                CyberpunkVR_HudCapWord,
                (unsigned long long)CyberpunkVR_HudCapBits,
                CyberpunkVR_HudInVrcam, CyberpunkVR_HudBorrowBlocks,
                CyberpunkVR_HudGrantCap);
        }
    }

    // Lend MAIN's draw-block list to the second eye for the duration of the HUD node.
    // See g_hud_block_main: this is the exit the node actually takes, and the slot is restored
    // below whatever the node does.
    void** hud_block_slot = nullptr;
    void*  hud_block_saved = nullptr;
    if (work_rva == DRAWHUD_WORK_RVA && work_context) {
        if (!g_hud_viewdata_get && g_exe_base) {
            g_hud_viewdata_get = reinterpret_cast<HudViewDataFn>(g_exe_base + 0x1ED930);
        }
        if (g_hud_viewdata_get) {
            __try {
                uint8_t* vd = reinterpret_cast<uint8_t*>(g_hud_viewdata_get(work_context));
                if (vd) {
                    void** slot = reinterpret_cast<void**>(vd + 0x168);
                    void* cur = *slot;
                    if (!vrcam_node) {
                        // MAIN's own HUD node, same frame, same node: the most honest source
                        // for the list the second eye is missing.
                        if (cur) g_hud_block_main = cur;
                    } else if (!cur) {
                        InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(
                            &CyberpunkVR_DebugHudBlockNull));
                        if (CyberpunkVR_HudInVrcam && CyberpunkVR_HudBorrowBlocks &&
                            g_hud_block_main) {
                            hud_block_slot = slot;
                            hud_block_saved = cur;
                            *slot = g_hud_block_main;
                            InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(
                                &CyberpunkVR_DebugHudBlockLent));
                        }
                    } else {
                        // Not empty -- then this is NOT where the node stops, and the borrow is
                        // the wrong fix. Counted so that shows up instead of being assumed.
                        InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(
                            &CyberpunkVR_DebugHudBlockOk));
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { hud_block_slot = nullptr; }
        }
    }

    const bool previous_vrcam_node = t_vrcam_node_active;
    t_vrcam_node_active = vrcam_node;
    // Publish the exact view for the camera hooks that run inside this dispatch. Saved and
    // restored like the vrcam flag: SceneDrv re-enters the dispatcher per pass, so a nested
    // node must not leave the parent's view mis-tagged.
    const bool     previous_view_known = t_active_view_known;
    const uint64_t previous_view_key   = t_active_view_key;
    t_active_view_known = view_key_known;
    t_active_view_key   = view_key;

    // Profile EVERY depth, not just the outermost. SceneDrv (+0x1EC1D0) drives ~37 scene
    // passes back through this same hook, so a depth==0 guard gives those child nodes ZERO
    // rows and buries their cost inside the parent -- which is why RenderElements & co were
    // invisible in the profiler and only showed up in the (now dead) array harness.
    // We therefore time all depths and record BOTH inclusive and self time, using a
    // thread-local accumulator through which each node reports its inclusive time upward.
    int64_t prof_t0 = 0, prof_saved_child = 0;
    const bool prof_on = CyberpunkVR_ProfEnable != 0;
    const bool prof_top = prof_on && t_prof_disp_depth == 0;
    ++t_prof_disp_depth;
    if (prof_on) {
        prof_saved_child = t_prof_child_ticks;
        t_prof_child_ticks = 0;
        prof_t0 = prof_now();
    }
    const uint8_t result = g_node_dispatch_orig(node, work_context, args);
    // Give the slot back before anything else can run on this view.
    if (hud_block_slot) {
        __try { *hud_block_slot = hud_block_saved; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (prof_on) {
        const int64_t dt = prof_now() - prof_t0;
        int64_t self = dt - t_prof_child_ticks;
        if (self < 0) self = 0;                       // clock jitter across cores
        t_prof_child_ticks = prof_saved_child + dt;   // hand our inclusive time to the parent
        if (prof_top) {   // frame totals stay top-level only, else they double-count
            if (vrcam_node) {
                g_prof_disp_vrcam_ticks.fetch_add(dt, std::memory_order_relaxed);
                g_prof_disp_vrcam_nodes.fetch_add(1, std::memory_order_relaxed);
                g_prof_top_vrcam.fetch_add(1, std::memory_order_relaxed);
            } else {
                g_prof_disp_main_ticks.fetch_add(dt, std::memory_order_relaxed);
                g_prof_disp_main_nodes.fetch_add(1, std::memory_order_relaxed);
                g_prof_top_main.fetch_add(1, std::memory_order_relaxed);
            }
        }
        prof_node_add(prof_work, dt, self, vrcam_node);
        prof_pair_add(scene_rtid, work_rva, self, vrcam_node, !prof_top, node_owner_bit);
        if (work_rva == SCENE_DRIVER_WORK_RVA) {
            ProfPass& p = g_prof_scenepass[scene_rtid];
            if (vrcam_node) {
                p.ticks_vrcam.fetch_add(dt, std::memory_order_relaxed);
                p.calls_vrcam.fetch_add(1, std::memory_order_relaxed);
            } else {
                p.ticks_main.fetch_add(dt, std::memory_order_relaxed);
                p.calls_main.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    --t_prof_disp_depth;

    t_vrcam_node_active = previous_vrcam_node;
    t_active_view_known = previous_view_known;
    t_active_view_key   = previous_view_key;
    t_current_node_work = previous_node_work;
    // Tonemap OUTPUT snapshot -> our committed g_stable_tex (flicker fix). Fires at the
    // tonemap node's own epilogue while its list is still open and RT0 not yet aliased.
    if (CyberpunkVR_StableFromTonemap && !t_tm_consumed && t_tm_rt0 && t_tm_rt0_list &&
            CyberpunkVR_StableCopy && stereo_eye_capture_wanted()) {
        mirror_stable_inline_copy(t_tm_rt0_list, t_tm_rt0, t_tm_rt0_state);
        t_tm_consumed = true;
        g_have_tonemap_source.store(true, std::memory_order_release);
        InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(
            &CyberpunkVR_DebugTonemapSnaps));
    }
    ID3D12Resource* const mirror_output = mirror_copy_node ? t_mirror_copy_rtv : nullptr;
    const DXGI_FORMAT mirror_output_format = mirror_copy_node ? t_mirror_copy_rtv_format
                                                             : DXGI_FORMAT_UNKNOWN;
    ID3D12GraphicsCommandList* const mirror_list = mirror_copy_node ? t_mirror_copy_list : nullptr;
    const uint32_t mirror_src_state = t_mirror_src_state;
    if (mirror_copy_node) {
        t_mirror_copy_node_active = previous_mirror_active;
        t_mirror_copy_rtv = previous_mirror_rtv;
        t_mirror_copy_rtv_format = previous_mirror_format;
        t_mirror_copy_list = previous_mirror_list;
    }
    if (mirror_output) {
        // Valid-window snapshot: the node work-fn just returned, so the final write is
        // recorded on mirror_list and no later pass aliased it yet. Final2D is the
        // flapping source -> skip it only when a tonemap snapshot is actually available.
        const bool tonemap_src = CyberpunkVR_StableFromTonemap &&
            g_have_tonemap_source.load(std::memory_order_acquire);
        if (CyberpunkVR_StableCopy && stereo_eye_capture_wanted() && mirror_list && !tonemap_src) {
            g_eye_copy_calls.fetch_add(1, std::memory_order_relaxed);
            mirror_stable_inline_copy(mirror_list, mirror_output, mirror_src_state);
        } else if (CyberpunkVR_StableCopy && stereo_eye_capture_wanted() && !tonemap_src) {
            // The output target was found but there is no command list to record the copy on.
            // publish() below does not need one, which is exactly why this case can starve the
            // eye while every existing diagnostic reports health.
            g_eye_no_list.fetch_add(1, std::memory_order_relaxed);
        }
        mirror_publish_output(mirror_output, mirror_output_format);
        const uint64_t serial = g_mirror_vrcam_serial.load(std::memory_order_acquire);
        uint64_t armed = g_mirror_armed_serial.load(std::memory_order_relaxed);
        if (serial && armed != serial &&
            g_mirror_armed_serial.compare_exchange_strong(
                armed, serial, std::memory_order_acq_rel)) {
            g_mirror_copy_armed.store(true, std::memory_order_release);
            InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(
                &CyberpunkVR_DebugMirrorCopyArms));
        }
    } else if (mirror_copy_node) {
        // The node ran and bound nothing we recognised as its output. Nothing downstream fires --
        // not the snapshot, not publish -- so this is the one branch that is silent everywhere.
        g_eye_no_rtv.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}


// --- Descriptor-heap probe / enlarge (Path A) ------------------------------
using PFN_D3D12CreateDevice = HRESULT (WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL,
                                                REFIID, void**);
using PFN_CreateDescriptorHeap = HRESULT (STDMETHODCALLTYPE*)(
    ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);

PFN_D3D12CreateDevice     g_orig_D3D12CreateDevice = nullptr;
PFN_CreateDescriptorHeap  g_orig_CreateDescriptorHeap = nullptr;
std::atomic<bool>         g_desc_probe_installed{false};
std::atomic<bool>         g_desc_vtable_patched{false};
// Enlarge the shader-visible CBV_SRV_UAV heap so two full views fit. RTX Tier 3
// allows >1,000,000 (bounded by VRAM). Kept modest; only applied to the big
// shader-visible heap. NOTE: on its own this only grows the real heap; the
// engine's internal budget still needs the matching site patch  this build is
// primarily to LOG the exact requesting site (caller RVA).
bool     g_enable_desc_heap_enlarge = false;
uint32_t g_desc_heap_target = 2000000u;

// Boot-time engine constant patch: sub_14091D604 builds the shader-visible
// CBV_SRV_UAV heap size as `0xDC240 - v4` (sub-allocator budget = heap+0x10) and
// NumDescriptors = budget + (v4 + 0x18000) = 0xDC240 + 0x18000 = 1,000,000.
// Raising the single base constant 0xDC240 -> 0x1DC240 scales BOTH the engine
// budget (868,928 -> 1,917,504) and the real D3D12 heap (1,000,000 -> 2,048,576)
// consistently, so two full views fit. RTX 5070 Ti is Resource Binding Tier 3.
// Instruction: 0x91D64A  B8 40 C2 0D 00  mov eax, 0xDC240  (imm32 at +1).
constexpr uintptr_t DESC_HEAP_SIZE_MOV_RVA = 0x91D64A;
constexpr uint32_t  DESC_HEAP_SIZE_ORIG    = 0x000DC240u;
constexpr uint32_t  DESC_HEAP_SIZE_NEW     = 0x001DC240u;
// Disabled: raising the shader-visible CBV_SRV_UAV heap past 1,000,000 is
// rejected by the D3D12 runtime/driver (CreateDescriptorHeap returns null ->
// engine null-derefs at boot). Confirmed live: num=2,048,576 crashed. The 1M
// shader-visible cap is hard here; enlargement is not viable. Keep OFF.
bool     g_enable_desc_heap_resize = false;
std::atomic<bool>   g_desc_heap_resized{false};
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugDescHeapResized = 0;


static void patch_descriptor_heap_size() {
    if (!g_enable_desc_heap_resize) return;
    bool expected = false;
    if (!g_desc_heap_resized.compare_exchange_strong(expected, true)) return;
    if (!g_exe_base) sync_stereo_init();
    uint8_t* mov = g_exe_base + DESC_HEAP_SIZE_MOV_RVA;
    __try {
        if (mov[0] != 0xB8 ||
            *reinterpret_cast<uint32_t*>(mov + 1) != DESC_HEAP_SIZE_ORIG) {
            CyberpunkVR_DebugDescHeapResized = 0xFFFFFFFFu; // validation failed
            log("[descheap] size-const validation FAILED at %p (op=0x%02X imm=0x%X)",
                mov, mov[0], *reinterpret_cast<uint32_t*>(mov + 1));
            return;
        }
        DWORD oldp = 0;
        if (!VirtualProtect(mov + 1, 4, PAGE_EXECUTE_READWRITE, &oldp)) return;
        *reinterpret_cast<uint32_t*>(mov + 1) = DESC_HEAP_SIZE_NEW;
        DWORD junk = 0;
        VirtualProtect(mov + 1, 4, oldp, &junk);
        FlushInstructionCache(GetCurrentProcess(), mov, 5);
        CyberpunkVR_DebugDescHeapResized = DESC_HEAP_SIZE_NEW;
        log("[descheap] heap size const patched 0x%X -> 0x%X at %p",
            DESC_HEAP_SIZE_ORIG, DESC_HEAP_SIZE_NEW, mov);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        CyberpunkVR_DebugDescHeapResized = 0xEEEEEEEEu;
    }
}

static HRESULT STDMETHODCALLTYPE Hook_CreateDescriptorHeap(
        ID3D12Device* self, const D3D12_DESCRIPTOR_HEAP_DESC* desc,
        REFIID riid, void** out) {
    const uintptr_t ret_abs = reinterpret_cast<uintptr_t>(_ReturnAddress());
    CyberpunkVR_DebugDescHeapCreates++;
    D3D12_DESCRIPTOR_HEAP_DESC local;
    const D3D12_DESCRIPTOR_HEAP_DESC* use = desc;
    if (desc) {
        const bool shader_visible =
            (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0;
        if (desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
            shader_visible && desc->NumDescriptors >= 0x40000u) {
            uintptr_t rva = ret_abs - reinterpret_cast<uintptr_t>(g_exe_base);
            CyberpunkVR_DebugDescHeapSVNum = desc->NumDescriptors;
            CyberpunkVR_DebugDescHeapSVRetRva = rva;
            CyberpunkVR_DebugDescHeapSVRetAbs = ret_abs;
            CyberpunkVR_DebugDescHeapSVFlags = desc->Flags;
            log("[descheap] shader-visible CBV_SRV_UAV num=%u flags=0x%X caller_abs=0x%llX rva=0x%llX",
                desc->NumDescriptors, desc->Flags,
                (unsigned long long)ret_abs, (unsigned long long)rva);
            if (g_enable_desc_heap_enlarge && desc->NumDescriptors < g_desc_heap_target) {
                local = *desc;
                local.NumDescriptors = g_desc_heap_target;
                use = &local;
                CyberpunkVR_DebugDescHeapEnlarged++;
                log("[descheap] enlarged -> %u", g_desc_heap_target);
            }
        }
    }
    return g_orig_CreateDescriptorHeap(self, use, riid, out);
}

// --- ExecuteCommandLists probe: count actual GPU command-list executions per
// frame. Definitive test for "does the eye execute?": if execLists/frame ~doubles
// under stereo, the eye's command lists really run on the GPU. Queue vtable slot
// 10 = ExecuteCommandLists; device vtable slot 8 = CreateCommandQueue.
bool g_enable_exec_probe = true;
std::atomic<uint64_t> g_exec_total{0};        // cumulative command lists executed
std::atomic<uint64_t> g_exec_last_frame{0};
std::atomic<uint64_t> g_exec_frame_ctr{0};
std::atomic<bool>     g_queue_vtable_patched{false};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugExecTotal    = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugExecPerFrame = 0;

using PFN_ExecuteCommandLists =
    void (STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using PFN_CreateCommandQueue =
    HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
PFN_ExecuteCommandLists g_orig_ExecuteCommandLists = nullptr;
PFN_CreateCommandQueue  g_orig_CreateCommandQueue  = nullptr;
using PFN_CreateCommandList = HRESULT (STDMETHODCALLTYPE*)
    (ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*,
     ID3D12PipelineState*, REFIID, void**);
using PFN_ResourceBarrier = void (STDMETHODCALLTYPE*)
    (ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using PFN_CopyResource = void (STDMETHODCALLTYPE*)
    (ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
using PFN_OMSetRenderTargets = void (STDMETHODCALLTYPE*)
    (ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*,
     BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
// Slot 14. Used only to NAME the node behind a dispatch: the light-tile pass was identified in
// a capture by its group count, but a capture cannot say which frame-graph node issued it.
using PFN_Dispatch = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
static void STDMETHODCALLTYPE hk_Dispatch(ID3D12GraphicsCommandList*, UINT, UINT, UINT);

using PFN_ExecuteIndirect = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
    ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64);
static void STDMETHODCALLTYPE hk_ExecuteIndirect(ID3D12GraphicsCommandList*,
    ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64);

// Direct draws, vtable slots 12 and 13. The dispatch census (slot 14) and the indirect census
// (slot 59) between them still miss ordinary geometry, which is what per-object effects like the
// scanner's object highlight are drawn with -- so "which nodes draw for MAIN and never for VRCAM"
// was unanswerable. Same shape as the other two censuses: aggregate per node, report the
// asymmetry, never bin by argument values (that produced a false lead once already).
using PFN_DrawInstanced = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
    UINT, UINT, UINT, UINT);
using PFN_DrawIndexedInstanced = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
    UINT, UINT, UINT, INT, UINT);
static void STDMETHODCALLTYPE hk_DrawInstanced(ID3D12GraphicsCommandList*,
    UINT, UINT, UINT, UINT);
// Slot 25. Needed only to know WHICH pso a draw runs under: the holographic sight's reticle is
// one specific pixel shader, and a shader can only be substituted at PSO-creation time, so the
// creation site has to be told which one to substitute. Identity travels as the PS bytecode
// hash, which is stable across runs; the pointer is not.
using PFN_SetPipelineState = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
    ID3D12PipelineState*);
static void STDMETHODCALLTYPE hk_SetPipelineState(ID3D12GraphicsCommandList*,
    ID3D12PipelineState*);
// Slot 44. The sight quad's placement rides in the instance stream at slot 7; reading it at the
// draw is the only way to compare the weapon's ORIENTATION between the two views, which is now
// the one remaining input that can differ. (The collimated direction is eye-position independent
// by construction, so a per-eye disagreement can only come from per-view instance data.)
using PFN_IASetVertexBuffers = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
    UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*);
static void STDMETHODCALLTYPE hk_IASetVertexBuffers(ID3D12GraphicsCommandList*,
    UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*);
static void STDMETHODCALLTYPE hk_DrawIndexedInstanced(ID3D12GraphicsCommandList*,
    UINT, UINT, UINT, INT, UINT);

using PFN_CopyTextureRegion = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
    const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
    const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);

using PFN_CopyBufferRegion = void (STDMETHODCALLTYPE*)
    (ID3D12GraphicsCommandList*, ID3D12Resource*, UINT64, ID3D12Resource*,
     UINT64, UINT64);
// vrcam post-DLSS crop fix: command-list viewport/scissor + reset hooks (slots 21/22/10).
using PFN_RSSetViewports = void (STDMETHODCALLTYPE*)
    (ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
using PFN_RSSetScissorRects = void (STDMETHODCALLTYPE*)
    (ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*);
using PFN_GfxReset = HRESULT (STDMETHODCALLTYPE*)
    (ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using CreateRTVFn = void (STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*,
    const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
PFN_CreateCommandList    g_orig_CreateCommandList = nullptr;
PFN_ResourceBarrier      g_orig_ResourceBarrier = nullptr;
PFN_CopyResource         g_orig_CopyResource = nullptr;
static CreateRTVFn g_orig_CreateRTV = nullptr;
// Defined further down, with the constant-block probe it serves; declared here because the
// device vtable is patched long before that point.
using CreateCBVFn = void (STDMETHODCALLTYPE*)(ID3D12Device*,
    const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
static CreateCBVFn g_orig_CreateCBV = nullptr;
static void STDMETHODCALLTYPE hk_CreateCBV(ID3D12Device*,
    const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
// Exposure probe: defined with the rest of it further down, used by the barrier hook above it.
static void expo_probe_copy(ID3D12GraphicsCommandList*, ID3D12Resource*, bool);
static void expo_mirror(ID3D12GraphicsCommandList*, ID3D12Resource*, bool);
static void expo_probe_report();
static bool tile_is_grid(const D3D12_RESOURCE_DESC&);
static void cull_count_note(ID3D12GraphicsCommandList*, ID3D12Resource*, uint32_t, bool);
static void cull_count_report();
static void tile_probe_copy(ID3D12GraphicsCommandList*, ID3D12Resource*,
                            const D3D12_RESOURCE_DESC&, D3D12_RESOURCE_STATES, bool);
static void tile_probe_report();
static std::atomic<bool> g_rtv_hook_installed{false};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMirrorGameQueue = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMirrorReadyFence = 0;

static std::mutex g_game_object_mtx;
static ID3D12Device* g_game_device = nullptr;
static ID3D12CommandQueue* g_game_queue = nullptr;
// Accessors for the game-view ImGui overlay (overlay_imgui.cpp).
extern "C" ID3D12Device*       CyberpunkVR_GetGameDevice() { return g_game_device; }
extern "C" ID3D12CommandQueue* CyberpunkVR_GetGameQueue()  { return g_game_queue; }

// Defined with the mirror capture code below; these are used by the real-device
// command-list hooks installed before the vrcam resource is created.
static ID3D12Resource* g_mirror_stage = nullptr;
static D3D12_RESOURCE_DESC g_mirror_stage_desc{};
static ID3D12Fence* g_mirror_game_fence = nullptr;
static ID3D12CommandAllocator* g_mirror_game_copy_allocator = nullptr;
static ID3D12GraphicsCommandList* g_mirror_game_copy_list = nullptr;
static uint64_t g_mirror_game_copy_inflight = 0;
static std::mutex g_mirror_game_copy_mtx;
static std::atomic<uint64_t> g_mirror_game_fence_next{1};
static std::atomic<uint64_t> g_mirror_ready_fence{0};
static std::atomic<ID3D12GraphicsCommandList*> g_mirror_pending_list{nullptr};

static void STDMETHODCALLTYPE hk_CreateRTV(ID3D12Device*, ID3D12Resource*,
        const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
static void STDMETHODCALLTYPE hk_ResourceBarrier(ID3D12GraphicsCommandList*,
        UINT, const D3D12_RESOURCE_BARRIER*);
static void STDMETHODCALLTYPE hk_OMSetRenderTargets(ID3D12GraphicsCommandList*,
        UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
        const D3D12_CPU_DESCRIPTOR_HANDLE*);
static void STDMETHODCALLTYPE hk_CopyBufferRegion(ID3D12GraphicsCommandList*,
        ID3D12Resource*, UINT64, ID3D12Resource*, UINT64, UINT64);
static void STDMETHODCALLTYPE hk_RSSetViewports(ID3D12GraphicsCommandList*,
        UINT, const D3D12_VIEWPORT*);
static void STDMETHODCALLTYPE hk_RSSetScissorRects(ID3D12GraphicsCommandList*,
        UINT, const D3D12_RECT*);
static HRESULT STDMETHODCALLTYPE hk_GfxReset(ID3D12GraphicsCommandList*,
        ID3D12CommandAllocator*, ID3D12PipelineState*);
// POST-DLSS CROP FIX (D3D12-level, definitive): per Nsight, the crop is a vrcam-only post-DLSS
// fullscreen blit/tonemap (PipelineState_563) that READS the 2444 DLSS output and WRITES a 2444
// RT but with a render-res (1418) viewport+scissor -> only the top-left 1418 is filled = crop.
// MAIN has no such graphics pass (it composites via compute). The blit's viewport is NOT the
// view render-res field we can flip (VP+0x34 feeds pre-DLSS passes too), so we correct it at the
// D3D12 layer: on the SAME command-list/thread that records the vrcam DLSS eval, any viewport or
// scissor set to the vrcam RENDER size (1418) AFTER the eval is upscaled to the vrcam OUTPUT size
// (2444). Thread-local phase (set at vrcam eval, cleared at the command-list Reset that begins the
// next frame's recording) keeps this from touching pre-DLSS passes; the render-size match keeps it
// off MAIN (main never uses the square vrcam render size). Dims come from g_vrcam_dlss_r*/o*.
thread_local bool t_vrcam_dlss_post = false;   // (legacy, unused) vrcam post-DLSS phase marker
// THREAD-AGNOSTIC crop fix: OMSetRenderTargets and RSSetViewports/ScissorRects for a given draw
// run consecutively on the SAME command-list recording thread. So we capture the primary bound
// RT's dimensions per-thread here, and the viewport/scissor hooks upscale a viewport that
// under-fills that RT. This does not depend on which thread the DLSS eval ran on (the eval and
// the vrcam blit are NOT reliably co-threaded in the live frame graph).
thread_local UINT t_cur_rt_w = 0;   // primary bound RT width  on THIS thread
thread_local UINT t_cur_rt_h = 0;   // primary bound RT height on THIS thread
// Set while the vrcam CopyToTexture (sub_140377B58) node is recording on THIS thread: the crop
// pass whose viewport comes from a render-res SOURCE resource (not VP+0x34), so the VP-swap alone
// can't move it. While set, hk_RSSetViewports/Scissor upscale the render-res (1418) viewport to
// output (2444) -> the copy fills the whole 2444 target. Scoped to just this one node + vrcam.
thread_local bool t_copytotex = false;
// (removed: investigation-only viewport/blit diagnostics -- crop root FOUND = raster tonemap
//  sub_140768510 gated by group 20; fixed natively in Detour_RenderRes via view+0x17D0 match-main.
//  The stack-capture + band-aid are no longer needed, and the per-viewport RtlCaptureStackBackTrace
//  was pure overhead.)
// Broad RTV -> dims map (ANY format/size, unlike the RGBA8-only mirror candidate list) so the
// OMSetRenderTargets hook can tell the size of the bound RT for the R11G11B10 DLSS-output blit.
// `res` so a bound RTV can be resolved back to its texture -- needed to name the surface
// RenderVisionElements draws the scanner's object outline into (see the vision snapshot).
// EVERY RTV THE GAME CREATES, BY DESCRIPTOR HANDLE. Read on the recording threads to answer "what
// is this bind pointing at", which is how the HUD surface is identified and how the vrcam output
// is recognised -- so a lookup that comes back empty does not degrade anything, it switches a
// feature off.
//
// It used to be 2048 entries with `if (n >= size) return;` -- silently stop accepting, forever.
// Past that point every newly created descriptor was invisible, and a frame-graph rebuild creates
// a whole new set: the HUD node kept binding its surface and we could no longer say what the
// handle meant, so `[hud] surface named by DrawHUD` stopped appearing and the second eye lost the
// HUD for the session. The same saturation is why a [rtvpick] miss reported "descriptor never seen
// created" for a target the engine had plainly just created.
//
// A ring now, and it says so when it first wraps. Overwriting the OLDEST entry is the right trade
// here: a descriptor that has not been re-created in eight thousand creations is one the engine has
// almost certainly recycled anyway, and the loop below already refreshes an entry in place when its
// handle comes round again.
//
// `handle` is atomic so publication is ordered rather than hoped for: the writer clears it, fills
// the rest, then stores the handle last; a reader that sees the handle therefore sees the fields
// that go with it. Readers do not take the mutex -- this is consulted on every OMSetRenderTargets.
struct RtvDimEntry {
    std::atomic<SIZE_T> handle{0};
    uint32_t w = 0, h = 0;
    ID3D12Resource* res = nullptr;
};
static std::array<RtvDimEntry, 8192> g_rtv_dim_map{};
static std::atomic<uint32_t> g_rtv_dim_count{0};
static std::atomic<uint32_t> g_rtv_dim_next{0};
static bool g_rtv_dim_wrapped_logged = false;
// How often the HUD node binds a target, and how often we cannot say what that bind points at.
// The second number rising with the first is the map above failing to answer, which is the whole
// difference between "the HUD moved" and "we went blind to it".
static std::atomic<uint64_t> g_hud_node_binds{0};
static std::atomic<uint64_t> g_hud_node_unresolved{0};
static std::mutex g_rtv_dim_mtx;
static void d12_present_thread();
static void d12_submit_mirror_copy(ID3D12CommandQueue*);

static void STDMETHODCALLTYPE Hook_ExecuteCommandLists(
        ID3D12CommandQueue* self, UINT n, ID3D12CommandList* const* lists) {
    CyberpunkVR_DebugExecTotal = g_exec_total.fetch_add(n, std::memory_order_relaxed) + n;
    // Do the single 11on12 copy+present only once the game actually SUBMITS the
    // command list that wrote the vrcam dtex (the one that recorded the blit's
    // RENDER_TARGET->read barrier). Queue ordering then guarantees our copy reads
    // the freshly written frame. Non-blocking (Flush + Present(0)) => no FPS drop.
    ID3D12GraphicsCommandList* pending =
        g_mirror_pending_list.load(std::memory_order_acquire);
    bool submits_blit = false;
    if (pending && lists) {
        for (UINT i = 0; i < n; ++i) {
            if (lists[i] == reinterpret_cast<ID3D12CommandList*>(pending)) {
                submits_blit = true; break;
            }
        }
    }
    g_orig_ExecuteCommandLists(self, n, lists);
    // The game just submitted the list that wrote the vrcam final. That target rests
    // permanently in RENDER_TARGET (never read back -> no RT->read barrier exists),
    // so we copy it out ourselves with one tiny submit on the game queue right here,
    // from its known fixed state. Queue order guarantees we read the fresh frame.
    if (submits_blit && self == g_game_queue) {
        g_mirror_pending_list.store(nullptr, std::memory_order_release);
        d12_submit_mirror_copy(self);
    }
}

static void patch_queue_vtable(void* queue) {
    if (!queue) return;
    bool e = false;
    if (!g_queue_vtable_patched.compare_exchange_strong(e, true)) return;
    void** vt = *reinterpret_cast<void***>(queue);
    DWORD oldp = 0;
    if (VirtualProtect(&vt[10], sizeof(void*), PAGE_READWRITE, &oldp)) {
        g_orig_ExecuteCommandLists = reinterpret_cast<PFN_ExecuteCommandLists>(vt[10]);
        vt[10] = reinterpret_cast<void*>(&Hook_ExecuteCommandLists);
        DWORD junk = 0;
        VirtualProtect(&vt[10], sizeof(void*), oldp, &junk);
        log("[exec] ExecuteCommandLists hooked (queue=%p orig=%p)",
            queue, (void*)g_orig_ExecuteCommandLists);
    } else {
        g_queue_vtable_patched.store(false, std::memory_order_release);
    }
}

static HRESULT STDMETHODCALLTYPE Hook_CreateCommandQueue(
        ID3D12Device* self, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID riid, void** out) {
    HRESULT hr = g_orig_CreateCommandQueue(self, desc, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        auto* queue = reinterpret_cast<ID3D12CommandQueue*>(*out);
        if (desc && desc->Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            std::lock_guard<std::mutex> lock(g_game_object_mtx);
            if (!g_game_queue) {
                queue->AddRef();
                g_game_queue = queue;
                CyberpunkVR_DebugMirrorGameQueue = reinterpret_cast<uint64_t>(queue);
                log("[mirror] captured game DIRECT queue=%p device=%p", queue, self);
            }
        }
        patch_queue_vtable(queue);
    }
    return hr;
}

// Filled by the PSO-creation hooks below; consumed by the sight probe further down.
static void pso_ids_record(void*, const D3D12_SHADER_BYTECODE&, const D3D12_SHADER_BYTECODE&);
static void hud_adopt_by_node(ID3D12Resource*);
static std::atomic<uint64_t> g_hud_snap_tick{0};
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_HudNodeRva;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_HudNodeRva2;
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_HudByNode;
static uint64_t fnv1a(const void*, size_t);
static void sight_ps_dump(const void*, size_t, const char*);
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_SightPsHash;
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_SightPsDump;

struct CommandListVtableHook {
    void** vtable = nullptr;
    PFN_OMSetRenderTargets original = nullptr;          // slot 46 (hooked)
    PFN_ResourceBarrier    barrier_original = nullptr;  // slot 26 (hooked)
    PFN_ResourceBarrier    barrier_call = nullptr;      // slot 26 raw (for appending)
    PFN_CopyResource       copyres = nullptr;           // slot 17 raw (for appending)
    PFN_CopyTextureRegion  copytex = nullptr;           // slot 16 raw (tile-grid probe)
    PFN_ExecuteIndirect    indirect_original = nullptr; // slot 59 (hooked, see census)
    PFN_DrawInstanced        draw_original = nullptr;      // slot 12 (draw census)
    PFN_DrawIndexedInstanced drawidx_original = nullptr;   // slot 13 (draw census)
    PFN_CopyBufferRegion   cbr_original = nullptr;      // slot 15 (hooked, CB probe)
    PFN_Dispatch           dispatch_original = nullptr; // slot 14 (hooked, node naming)
    PFN_RSSetViewports     viewports_original = nullptr;// slot 21 (hooked, crop fix)
    PFN_RSSetScissorRects  scissor_original = nullptr;  // slot 22 (hooked, crop fix)
    PFN_GfxReset           reset_original = nullptr;    // slot 10 (hooked, phase reset)
    PFN_SetPipelineState   setpso_original = nullptr;   // slot 25 (hooked, PSO probe)
    PFN_IASetVertexBuffers iavb_original = nullptr;     // slot 44 (hooked, sight axis probe)
};
static std::array<CommandListVtableHook, 16> g_command_list_vtable_hooks{};
static std::atomic<uint32_t> g_command_list_vtable_hook_count{0};
static std::mutex g_command_list_vtable_hook_mtx;

static const CommandListVtableHook* command_list_hook_entry(
        ID3D12GraphicsCommandList* command_list) {
    if (!command_list) return nullptr;
    void** vtable = *reinterpret_cast<void***>(command_list);
    const uint32_t count =
        g_command_list_vtable_hook_count.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count; ++i) {
        if (g_command_list_vtable_hooks[i].vtable == vtable)
            return &g_command_list_vtable_hooks[i];
    }
    return nullptr;
}
static PFN_OMSetRenderTargets command_list_original_om(
        ID3D12GraphicsCommandList* command_list) {
    const CommandListVtableHook* e = command_list_hook_entry(command_list);
    return e ? e->original : nullptr;
}

static void patch_command_list_vtable(void* command_list) {
    if (!command_list) return;
    void** vtable = *reinterpret_cast<void***>(command_list);
    std::lock_guard<std::mutex> lock(g_command_list_vtable_hook_mtx);
    uint32_t count =
        g_command_list_vtable_hook_count.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < count; ++i) {
        if (g_command_list_vtable_hooks[i].vtable == vtable) return;
    }
    if (count >= g_command_list_vtable_hooks.size()) return;
    PFN_OMSetRenderTargets om_orig = nullptr;
    PFN_ResourceBarrier    rb_orig = nullptr;
    DWORD oldp = 0;
    if (VirtualProtect(&vtable[46], sizeof(void*), PAGE_READWRITE, &oldp)) {
        om_orig = reinterpret_cast<PFN_OMSetRenderTargets>(vtable[46]);
        vtable[46] = reinterpret_cast<void*>(&hk_OMSetRenderTargets);
        DWORD junk = 0; VirtualProtect(&vtable[46], sizeof(void*), oldp, &junk);
    } else {
        return;
    }
    DWORD oldp2 = 0;
    if (VirtualProtect(&vtable[26], sizeof(void*), PAGE_READWRITE, &oldp2)) {
        rb_orig = reinterpret_cast<PFN_ResourceBarrier>(vtable[26]);
        vtable[26] = reinterpret_cast<void*>(&hk_ResourceBarrier);
        DWORD junk = 0; VirtualProtect(&vtable[26], sizeof(void*), oldp2, &junk);
    }
    PFN_CopyBufferRegion cbr_orig = nullptr;
    DWORD oldp3 = 0;
    if (VirtualProtect(&vtable[15], sizeof(void*), PAGE_READWRITE, &oldp3)) {
        cbr_orig = reinterpret_cast<PFN_CopyBufferRegion>(vtable[15]);
        vtable[15] = reinterpret_cast<void*>(&hk_CopyBufferRegion);
        DWORD junk = 0; VirtualProtect(&vtable[15], sizeof(void*), oldp3, &junk);
    }
    PFN_ExecuteIndirect ind_orig = nullptr;
    DWORD oldpI = 0;
    if (VirtualProtect(&vtable[59], sizeof(void*), PAGE_READWRITE, &oldpI)) {
        ind_orig = reinterpret_cast<PFN_ExecuteIndirect>(vtable[59]);
        vtable[59] = reinterpret_cast<void*>(&hk_ExecuteIndirect);
        DWORD junk = 0; VirtualProtect(&vtable[59], sizeof(void*), oldpI, &junk);
    }
    PFN_Dispatch disp_orig = nullptr;
    DWORD oldpD = 0;
    if (VirtualProtect(&vtable[14], sizeof(void*), PAGE_READWRITE, &oldpD)) {
        disp_orig = reinterpret_cast<PFN_Dispatch>(vtable[14]);
        vtable[14] = reinterpret_cast<void*>(&hk_Dispatch);
        DWORD junk = 0; VirtualProtect(&vtable[14], sizeof(void*), oldpD, &junk);
    }
    // POST-DLSS CROP FIX: RSSetViewports(21), RSSetScissorRects(22), Reset(10).
    PFN_RSSetViewports    vp_orig  = nullptr;
    PFN_RSSetScissorRects sc_orig  = nullptr;
    PFN_GfxReset          rst_orig = nullptr;
    DWORD oldp4 = 0;
    if (VirtualProtect(&vtable[21], sizeof(void*), PAGE_READWRITE, &oldp4)) {
        vp_orig = reinterpret_cast<PFN_RSSetViewports>(vtable[21]);
        vtable[21] = reinterpret_cast<void*>(&hk_RSSetViewports);
        DWORD junk = 0; VirtualProtect(&vtable[21], sizeof(void*), oldp4, &junk);
    }
    DWORD oldp5 = 0;
    if (VirtualProtect(&vtable[22], sizeof(void*), PAGE_READWRITE, &oldp5)) {
        sc_orig = reinterpret_cast<PFN_RSSetScissorRects>(vtable[22]);
        vtable[22] = reinterpret_cast<void*>(&hk_RSSetScissorRects);
        DWORD junk = 0; VirtualProtect(&vtable[22], sizeof(void*), oldp5, &junk);
    }
    DWORD oldp6 = 0;
    if (VirtualProtect(&vtable[10], sizeof(void*), PAGE_READWRITE, &oldp6)) {
        rst_orig = reinterpret_cast<PFN_GfxReset>(vtable[10]);
        vtable[10] = reinterpret_cast<void*>(&hk_GfxReset);
        DWORD junk = 0; VirtualProtect(&vtable[10], sizeof(void*), oldp6, &junk);
    }
    auto cr = reinterpret_cast<PFN_CopyResource>(vtable[17]);      // raw, for appending
    PFN_DrawInstanced dr_orig = nullptr;
    DWORD oldpDr = 0;
    if (VirtualProtect(&vtable[12], sizeof(void*), PAGE_READWRITE, &oldpDr)) {
        dr_orig = reinterpret_cast<PFN_DrawInstanced>(vtable[12]);
        vtable[12] = reinterpret_cast<void*>(&hk_DrawInstanced);
        DWORD junk = 0; VirtualProtect(&vtable[12], sizeof(void*), oldpDr, &junk);
    }
    PFN_DrawIndexedInstanced dri_orig = nullptr;
    DWORD oldpDi = 0;
    if (VirtualProtect(&vtable[13], sizeof(void*), PAGE_READWRITE, &oldpDi)) {
        dri_orig = reinterpret_cast<PFN_DrawIndexedInstanced>(vtable[13]);
        vtable[13] = reinterpret_cast<void*>(&hk_DrawIndexedInstanced);
        DWORD junk = 0; VirtualProtect(&vtable[13], sizeof(void*), oldpDi, &junk);
    }
    PFN_IASetVertexBuffers iavb_orig = nullptr;
    DWORD oldpVb = 0;
    if (VirtualProtect(&vtable[44], sizeof(void*), PAGE_READWRITE, &oldpVb)) {
        iavb_orig = reinterpret_cast<PFN_IASetVertexBuffers>(vtable[44]);
        vtable[44] = reinterpret_cast<void*>(&hk_IASetVertexBuffers);
        DWORD junk = 0; VirtualProtect(&vtable[44], sizeof(void*), oldpVb, &junk);
    }
    PFN_SetPipelineState sps_orig = nullptr;
    DWORD oldpSp = 0;
    if (VirtualProtect(&vtable[25], sizeof(void*), PAGE_READWRITE, &oldpSp)) {
        sps_orig = reinterpret_cast<PFN_SetPipelineState>(vtable[25]);
        vtable[25] = reinterpret_cast<void*>(&hk_SetPipelineState);
        DWORD junk = 0; VirtualProtect(&vtable[25], sizeof(void*), oldpSp, &junk);
    }
    auto ct = reinterpret_cast<PFN_CopyTextureRegion>(vtable[16]); // raw, tile-grid probe
    g_command_list_vtable_hooks[count] =
        {vtable, om_orig, rb_orig, rb_orig, cr, ct, ind_orig, dr_orig, dri_orig, cbr_orig,
         disp_orig, vp_orig, sc_orig, rst_orig, sps_orig, iavb_orig};
    g_command_list_vtable_hook_count.store(count + 1, std::memory_order_release);
    log("[mirror] command-list hooked list=%p vt=%p om=%p rb=%p cr=%p",
        command_list, vtable, (void*)om_orig, (void*)rb_orig, (void*)cr);
}

// ID3D12Device slots 27 and 29. Only to answer "which resource owns this GPU address" -- a
// vertex-buffer view carries an address and nothing else, and D3D12 offers no way back.
using PFN_CreateCommittedResource = HRESULT (STDMETHODCALLTYPE*)(
    ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using PFN_CreatePlacedResource = HRESULT (STDMETHODCALLTYPE*)(
    ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
    D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
static PFN_CreateCommittedResource g_orig_CreateCommitted = nullptr;
static PFN_CreatePlacedResource    g_orig_CreatePlaced = nullptr;
static void buf_note(ID3D12Resource*, uint64_t, uint64_t);

static void buf_note_created(void* out, const D3D12_RESOURCE_DESC* d) {
    if (!out || !d || d->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return;
    if (d->Width < 4096) return;                    // instance/vertex streams, not tiny scratch
    auto* res = static_cast<ID3D12Resource*>(out);
    uint64_t va = 0;
    __try { va = res->GetGPUVirtualAddress(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    buf_note(res, va, d->Width);
}

static HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource(
        ID3D12Device* self, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
        const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES st,
        const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out) {
    HRESULT hr = g_orig_CreateCommitted(self, hp, hf, d, st, cv, riid, out);
    if (SUCCEEDED(hr) && out && *out) buf_note_created(*out, d);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreatePlacedResource(
        ID3D12Device* self, ID3D12Heap* heap, UINT64 off, const D3D12_RESOURCE_DESC* d,
        D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out) {
    HRESULT hr = g_orig_CreatePlaced(self, heap, off, d, st, cv, riid, out);
    if (SUCCEEDED(hr) && out && *out) buf_note_created(*out, d);
    return hr;
}

// ID3D12Device slot 16. There is no reflection from an ID3D12RootSignature back to its
// description, so the only way to learn the binding contract our replacement shaders must live
// inside is to keep the blob at creation. What we are looking for is a root parameter this
// material does NOT use -- a spare root CBV or a set of root constants -- because occupying one
// of those needs no signature change at all: the PSO keeps the game's signature, nothing is
// rebound, and the muzzle direction can simply be set before the draw.
using PFN_CreateRootSignature = HRESULT (STDMETHODCALLTYPE*)(
    ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
static PFN_CreateRootSignature g_orig_CreateRootSig = nullptr;
static std::unordered_map<void*, std::vector<uint8_t>> g_rootsig_blobs;
static std::mutex g_rootsig_mtx;

static HRESULT STDMETHODCALLTYPE Hook_CreateRootSignature(
        ID3D12Device* self, UINT nodeMask, const void* blob, SIZE_T len,
        REFIID riid, void** out) {
    HRESULT hr = g_orig_CreateRootSig(self, nodeMask, blob, len, riid, out);
    if (SUCCEEDED(hr) && out && *out && blob && len && len < (1u << 20)) {
        std::lock_guard<std::mutex> lk(g_rootsig_mtx);
        if (g_rootsig_blobs.size() < 4096)
            g_rootsig_blobs[*out].assign(static_cast<const uint8_t*>(blob),
                                         static_cast<const uint8_t*>(blob) + len);
    }
    return hr;
}

// Print the layout once, for the signature the sight's pipeline uses.
static void rootsig_dump(void* rs) {
    if (!rs) return;
    static std::atomic<void*> s_done{nullptr};
    void* expected = nullptr;
    if (!s_done.compare_exchange_strong(expected, rs)) return;
    std::vector<uint8_t> blob;
    {
        std::lock_guard<std::mutex> lk(g_rootsig_mtx);
        auto it = g_rootsig_blobs.find(rs);
        if (it == g_rootsig_blobs.end()) {
            log("[rootsig] sight signature %p: blob not captured (created before the hook)", rs);
            return;
        }
        blob = it->second;
    }
    ID3D12VersionedRootSignatureDeserializer* des = nullptr;
    if (FAILED(D3D12CreateVersionedRootSignatureDeserializer(
            blob.data(), blob.size(), IID_PPV_ARGS(&des))) || !des) {
        log("[rootsig] sight signature %p: deserialize failed (%zu bytes)", rs, blob.size());
        return;
    }
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* v = nullptr;
    if (FAILED(des->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &v)) || !v) {
        des->Release();
        log("[rootsig] sight signature %p: no 1.1 view", rs);
        return;
    }
    const auto& d = v->Desc_1_1;
    log("[rootsig] sight signature %p: %u params, %u samplers, flags=0x%X",
        rs, d.NumParameters, d.NumStaticSamplers, (unsigned)d.Flags);
    for (UINT i = 0; i < d.NumParameters; ++i) {
        const auto& p = d.pParameters[i];
        const unsigned vis = (unsigned)p.ShaderVisibility;
        switch (p.ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            log("[rootsig]  [%2u] CONSTANTS b%u space%u x%u  vis=%u", i,
                p.Constants.ShaderRegister, p.Constants.RegisterSpace,
                p.Constants.Num32BitValues, vis);
            break;
        case D3D12_ROOT_PARAMETER_TYPE_CBV:
            log("[rootsig]  [%2u] CBV       b%u space%u  vis=%u", i,
                p.Descriptor.ShaderRegister, p.Descriptor.RegisterSpace, vis);
            break;
        case D3D12_ROOT_PARAMETER_TYPE_SRV:
            log("[rootsig]  [%2u] SRV       t%u space%u  vis=%u", i,
                p.Descriptor.ShaderRegister, p.Descriptor.RegisterSpace, vis);
            break;
        case D3D12_ROOT_PARAMETER_TYPE_UAV:
            log("[rootsig]  [%2u] UAV       u%u space%u  vis=%u", i,
                p.Descriptor.ShaderRegister, p.Descriptor.RegisterSpace, vis);
            break;
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
            char r[400]; int u = 0; r[0] = 0;
            for (UINT k = 0; k < p.DescriptorTable.NumDescriptorRanges && u < 340; ++k) {
                const auto& rг = p.DescriptorTable.pDescriptorRanges[k];
                const char* t = rг.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_CBV ? "b"
                              : rг.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV ? "t"
                              : rг.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV ? "u" : "s";
                u += snprintf(r + u, sizeof(r) - u, "%s%u+%u/sp%u ", t,
                              rг.BaseShaderRegister, rг.NumDescriptors, rг.RegisterSpace);
            }
            log("[rootsig]  [%2u] TABLE     vis=%u  %s", i, vis, r);
            break;
        }
        default: log("[rootsig]  [%2u] type=%u", i, (unsigned)p.ParameterType); break;
        }
    }
    des->Release();
}

// ID3D12Device slot 10 and ID3D12Device2 slot 47. Both are entry points the engine may use, and
// which one it takes is not worth guessing -- hooking both costs one pointer each. All they do
// here is remember (pso -> shader hashes); substitution, when it comes, happens in the same place.
using PFN_CreateGraphicsPipelineState = HRESULT (STDMETHODCALLTYPE*)(
    ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using PFN_CreatePipelineState = HRESULT (STDMETHODCALLTYPE*)(
    ID3D12Device*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
static PFN_CreateGraphicsPipelineState g_orig_CreateGfxPso = nullptr;
static PFN_CreatePipelineState         g_orig_CreatePso = nullptr;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPsoGfx = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPsoStream = 0;

// The replacement is loaded from a FILE rather than baked into the DLL. Iterating on it then
// costs a dxc run and a game restart instead of a full rebuild, and the shader stays readable
// next to the binary it patches.
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_SightPsSwap = 1;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugSightSwaps = 0;
// Measured: this pixel shader is used by TWO pipelines and only one of them draws the reticle.
// Keying the swap on the PAIR (pixel AND vertex hash) picks that one exactly, instead of relying
// on the other pipeline failing to build.
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_SightVsHash = 0x9228439BF72D91DBull;
static std::vector<uint8_t> g_sight_blob;      // pixel
static std::vector<uint8_t> g_sight_vs_blob;   // vertex
static std::atomic<int> g_sight_blob_state{0};   // 0 untried, 1 loaded, -1 missing

static bool sight_blob_ready() {
    int st = g_sight_blob_state.load(std::memory_order_acquire);
    if (st) return st > 0;
    static std::mutex mtx;
    std::lock_guard<std::mutex> lk(mtx);
    st = g_sight_blob_state.load(std::memory_order_relaxed);
    if (st) return st > 0;
    char path[MAX_PATH]{};
    HMODULE self = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&sight_blob_ready), &self) && self &&
        GetModuleFileNameA(self, path, MAX_PATH)) {
        char* slash = strrchr(path, '\\');
        if (slash) {
            *(slash + 1) = 0;
            const size_t dirLen = strlen(path);
            bool bothOk = true;
            for (int which = 0; which < 2 && bothOk; ++which) {
                path[dirLen] = 0;
                strcat_s(path, which ? "CyberpunkVR_SightVs.dxil" : "CyberpunkVR_SightPs.dxil");
                std::vector<uint8_t>& dstBlob = which ? g_sight_vs_blob : g_sight_blob;
                dstBlob.clear();
                HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (f != INVALID_HANDLE_VALUE) {
                    LARGE_INTEGER sz{};
                    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 64 && sz.QuadPart < (4 << 20)) {
                        dstBlob.resize(static_cast<size_t>(sz.QuadPart));
                        DWORD got = 0;
                        if (!(ReadFile(f, dstBlob.data(), static_cast<DWORD>(sz.QuadPart),
                                       &got, nullptr) && got == sz.QuadPart &&
                              memcmp(dstBlob.data(), "DXBC", 4) == 0)) {
                            dstBlob.clear();
                        }
                    }
                    CloseHandle(f);
                }
                if (dstBlob.empty()) {
                    log("[pso] sight replacement NOT found (%s) -- original shaders kept", path);
                    bothOk = false;
                }
            }
            // Both or neither: the pixel shader reads the glass size the vertex shader writes,
            // so half a swap would be worse than none.
            if (bothOk) {
                g_sight_blob_state.store(1, std::memory_order_release);
                log("[pso] sight replacement loaded: PS %zu B, VS %zu B",
                    g_sight_blob.size(), g_sight_vs_blob.size());
                return true;
            }
            g_sight_blob.clear();
            g_sight_vs_blob.clear();
        }
    }
    g_sight_blob_state.store(-1, std::memory_order_release);
    return false;
}

static bool sight_is_target(const D3D12_SHADER_BYTECODE& ps, const D3D12_SHADER_BYTECODE& vs) {
    return CyberpunkVR_SightPsSwap && ps.pShaderBytecode && ps.BytecodeLength &&
           vs.pShaderBytecode && vs.BytecodeLength &&
           fnv1a(ps.pShaderBytecode, ps.BytecodeLength) == CyberpunkVR_SightPsHash &&
           fnv1a(vs.pShaderBytecode, vs.BytecodeLength) == CyberpunkVR_SightVsHash &&
           sight_blob_ready();
}

static HRESULT STDMETHODCALLTYPE Hook_CreateGraphicsPipelineState(
        ID3D12Device* self, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
        REFIID riid, void** out) {
    if (desc && desc->PS.pShaderBytecode && desc->PS.BytecodeLength &&
        fnv1a(desc->PS.pShaderBytecode, desc->PS.BytecodeLength) == CyberpunkVR_SightPsHash) {
        rootsig_dump(desc->pRootSignature);
    }
    if (desc && sight_is_target(desc->PS, desc->VS)) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = *desc;
        d.PS.pShaderBytecode = g_sight_blob.data();
        d.PS.BytecodeLength = g_sight_blob.size();
        d.VS.pShaderBytecode = g_sight_vs_blob.data();
        d.VS.BytecodeLength = g_sight_vs_blob.size();
        HRESULT hr2 = g_orig_CreateGfxPso(self, &d, riid, out);
        if (SUCCEEDED(hr2)) {
            ++CyberpunkVR_DebugSightSwaps;
            log("[pso] sight PS substituted (graphics desc) pso=%p", out ? *out : nullptr);
            if (out && *out) pso_ids_record(*out, desc->PS, desc->VS);  // keep the ORIGINAL id
            ++CyberpunkVR_DebugPsoGfx;
            return hr2;
        }
        // A rejected replacement must not cost the game its shader: fall through to the original.
        log("[pso] sight PS substitution REFUSED hr=0x%08X -- keeping the original",
            static_cast<unsigned>(hr2));
    }
    HRESULT hr = g_orig_CreateGfxPso(self, desc, riid, out);
    if (SUCCEEDED(hr) && out && *out && desc) {
        pso_ids_record(*out, desc->PS, desc->VS);
        ++CyberpunkVR_DebugPsoGfx;
    }
    return hr;
}

// The stream form is a tagged blob: {alignas(void*) SUBOBJECT_TYPE, payload} repeated. Walking it
// is the only way to see the shaders, and the walk has to respect the pointer alignment between
// entries or it desynchronises and reads garbage.
// Offsets of the PS and VS payloads inside the tagged stream, SIZE_MAX when absent. Kept in its
// own function on purpose: it needs SEH, and SEH cannot share a frame with objects that unwind.
static void pso_stream_find(const uint8_t* p, size_t len, size_t* psoff, size_t* vsoff) {
    *psoff = SIZE_MAX; *vsoff = SIZE_MAX;
    if (!p || !len) return;
    const uint8_t* base = p;
    const uint8_t* end = p + len;
    __try {
        while (p + sizeof(void*) <= end) {
            const auto type = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*>(p);
            const uint8_t* payload = p + sizeof(void*);
            size_t plen = 0;
            switch (type) {
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
                plen = sizeof(D3D12_SHADER_BYTECODE);
                if (payload + plen > end) { p = end; break; }
                if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS)
                    *psoff = static_cast<size_t>(payload - base);
                else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS)
                    *vsoff = static_cast<size_t>(payload - base);
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
                plen = sizeof(void*); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
                plen = sizeof(D3D12_STREAM_OUTPUT_DESC); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
                plen = sizeof(D3D12_BLEND_DESC); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
                plen = sizeof(UINT); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
                plen = sizeof(D3D12_RASTERIZER_DESC); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
                plen = sizeof(D3D12_DEPTH_STENCIL_DESC); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
                plen = sizeof(D3D12_INPUT_LAYOUT_DESC); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
                plen = sizeof(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
                plen = sizeof(D3D12_PRIMITIVE_TOPOLOGY_TYPE); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
                plen = sizeof(D3D12_RT_FORMAT_ARRAY); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
                plen = sizeof(DXGI_FORMAT); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
                plen = sizeof(DXGI_SAMPLE_DESC); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
                plen = sizeof(UINT); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
                plen = sizeof(D3D12_CACHED_PIPELINE_STATE); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
                plen = sizeof(D3D12_PIPELINE_STATE_FLAGS); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
                plen = sizeof(D3D12_DEPTH_STENCIL_DESC1); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
                plen = sizeof(D3D12_VIEW_INSTANCING_DESC); break;
            default:
                // Unknown tag: the walk can no longer be trusted, so stop rather than
                // resynchronise on a guess.
                p = end; plen = 0; break;
            }
            if (p >= end) break;
            p += (sizeof(void*) + plen + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { *psoff = SIZE_MAX; *vsoff = SIZE_MAX; }
}


static HRESULT STDMETHODCALLTYPE Hook_CreatePipelineState(
        ID3D12Device* self, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
        REFIID riid, void** out) {
    // Same substitution as the classic path, but the stream is the caller's memory, so it is
    // COPIED first and the copy is patched -- writing into the engine's own description would
    // outlive this call and be visible to whatever else reads it.
    if (desc && desc->pPipelineStateSubobjectStream && desc->SizeInBytes) {
        const uint8_t* src = static_cast<const uint8_t*>(desc->pPipelineStateSubobjectStream);
        size_t psoff = SIZE_MAX, vsoff = SIZE_MAX;
        pso_stream_find(src, desc->SizeInBytes, &psoff, &vsoff);
        if (psoff != SIZE_MAX && vsoff != SIZE_MAX) {
            const auto& bc = *reinterpret_cast<const D3D12_SHADER_BYTECODE*>(src + psoff);
            const auto& bcv = *reinterpret_cast<const D3D12_SHADER_BYTECODE*>(src + vsoff);
            if (sight_is_target(bc, bcv)) {
                std::vector<uint8_t> copy(src, src + desc->SizeInBytes);
                auto& nbc = *reinterpret_cast<D3D12_SHADER_BYTECODE*>(copy.data() + psoff);
                nbc.pShaderBytecode = g_sight_blob.data();
                nbc.BytecodeLength = g_sight_blob.size();
                auto& nbv = *reinterpret_cast<D3D12_SHADER_BYTECODE*>(copy.data() + vsoff);
                nbv.pShaderBytecode = g_sight_vs_blob.data();
                nbv.BytecodeLength = g_sight_vs_blob.size();
                D3D12_PIPELINE_STATE_STREAM_DESC nd = *desc;
                nd.pPipelineStateSubobjectStream = copy.data();
                HRESULT hr2 = g_orig_CreatePso(self, &nd, riid, out);
                if (SUCCEEDED(hr2)) {
                    ++CyberpunkVR_DebugSightSwaps;
                    ++CyberpunkVR_DebugPsoStream;
                    log("[pso] sight PS substituted (stream desc) pso=%p", out ? *out : nullptr);
                    return hr2;
                }
                log("[pso] sight PS substitution REFUSED hr=0x%08X (stream) -- original kept",
                    static_cast<unsigned>(hr2));
            }
        }
    }
    HRESULT hr = g_orig_CreatePso(self, desc, riid, out);
    if (SUCCEEDED(hr) && out && *out && desc && desc->pPipelineStateSubobjectStream) {
        size_t psoff = SIZE_MAX, vsoff = SIZE_MAX;
        pso_stream_find(static_cast<const uint8_t*>(desc->pPipelineStateSubobjectStream),
                        desc->SizeInBytes, &psoff, &vsoff);
        D3D12_SHADER_BYTECODE ps{}, vs{};
        const uint8_t* base = static_cast<const uint8_t*>(desc->pPipelineStateSubobjectStream);
        if (psoff != SIZE_MAX) ps = *reinterpret_cast<const D3D12_SHADER_BYTECODE*>(base + psoff);
        if (vsoff != SIZE_MAX) vs = *reinterpret_cast<const D3D12_SHADER_BYTECODE*>(base + vsoff);
        if (ps.pShaderBytecode || vs.pShaderBytecode) pso_ids_record(*out, ps, vs);
        ++CyberpunkVR_DebugPsoStream;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateCommandList(
        ID3D12Device* self, UINT node_mask, D3D12_COMMAND_LIST_TYPE type,
        ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial_state,
        REFIID riid, void** out) {
    HRESULT hr = g_orig_CreateCommandList(
        self, node_mask, type, allocator, initial_state, riid, out);
    // COMPUTE as well as DIRECT. Registering only DIRECT lists left the engine's async-compute
    // list (`AsyncComputeDuringShadowmaps` in a capture) invisible to every hook here -- the
    // dispatch census, the ExecuteIndirect census, the barrier probes, all of them. That blind
    // spot is why the three 6x6x6 grading-volume builds each view does per frame never showed up
    // live even though both captures contain them.
    //
    // Safe to widen: command_list_hook_entry() keys on the VTABLE, not on the list pointer, so a
    // compute list simply finds the compute vtable's entry and the original is always called.
    // (A compute list has its own vtable, which is exactly why it was never patched before.)
    if (SUCCEEDED(hr) && out && *out &&
        (type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE))
        patch_command_list_vtable(*out);
    return hr;
}

static void patch_device_descriptor_slot(void* device) {
    if (!device) return;
    bool expected = false;
    if (!g_desc_vtable_patched.compare_exchange_strong(expected, true)) return;
    void** vt = *reinterpret_cast<void***>(device);
    // ID3D12Device slot 14 = CreateDescriptorHeap.
    DWORD oldp = 0;
    if (VirtualProtect(&vt[14], sizeof(void*), PAGE_READWRITE, &oldp)) {
        g_orig_CreateDescriptorHeap =
            reinterpret_cast<PFN_CreateDescriptorHeap>(vt[14]);
        vt[14] = reinterpret_cast<void*>(&Hook_CreateDescriptorHeap);
        DWORD junk = 0;
        VirtualProtect(&vt[14], sizeof(void*), oldp, &junk);
        log("[descheap] CreateDescriptorHeap hooked (dev=%p vt=%p orig=%p)",
            device, (void*)vt, (void*)g_orig_CreateDescriptorHeap);
    } else {
        g_desc_vtable_patched.store(false, std::memory_order_release);
        log("[descheap] FAILED to patch device vtable slot 14");
    }
    // ID3D12Device slot 8 = CreateCommandQueue (capture queues -> hook execute).
    if (g_enable_exec_probe) {
        DWORD o8 = 0;
        if (VirtualProtect(&vt[8], sizeof(void*), PAGE_READWRITE, &o8)) {
            g_orig_CreateCommandQueue = reinterpret_cast<PFN_CreateCommandQueue>(vt[8]);
            vt[8] = reinterpret_cast<void*>(&Hook_CreateCommandQueue);
            DWORD junk = 0;
            VirtualProtect(&vt[8], sizeof(void*), o8, &junk);
            log("[exec] CreateCommandQueue hooked (dev=%p)", device);
        }
    }
    // Slot 12 = CreateCommandList. Track each distinct D3D12Core command-list
    // vtable so OMSetRenderTargets can map the RTV bound specifically inside the
    // VRCAM CopyToTexture node. Slot 9 is CreateCommandAllocator (never patch it).
    DWORD o12 = 0;
    if (VirtualProtect(&vt[12], sizeof(void*), PAGE_READWRITE, &o12)) {
        g_orig_CreateCommandList = reinterpret_cast<PFN_CreateCommandList>(vt[12]);
        vt[12] = reinterpret_cast<void*>(&Hook_CreateCommandList);
        DWORD junk = 0;
        VirtualProtect(&vt[12], sizeof(void*), o12, &junk);
        log("[mirror] real device CreateCommandList hooked dev=%p orig=%p",
            device, (void*)g_orig_CreateCommandList);
    }
    // Slot 10 = CreateGraphicsPipelineState. Slot 47 = ID3D12Device2::CreatePipelineState, and
    // it is only touched when the device actually implements ID3D12Device2 -- patching a vtable
    // slot that may not exist is how a hook corrupts the object next to it.
    for (int k = 0; k < 2; ++k) {
        const UINT slot = k ? 29u : 27u;
        DWORD oldp = 0;
        if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &oldp)) continue;
        if (k) {
            g_orig_CreatePlaced = reinterpret_cast<PFN_CreatePlacedResource>(vt[slot]);
            vt[slot] = reinterpret_cast<void*>(&Hook_CreatePlacedResource);
        } else {
            g_orig_CreateCommitted = reinterpret_cast<PFN_CreateCommittedResource>(vt[slot]);
            vt[slot] = reinterpret_cast<void*>(&Hook_CreateCommittedResource);
        }
        DWORD junk = 0; VirtualProtect(&vt[slot], sizeof(void*), oldp, &junk);
    }
    log("[sightaxis] buffer-address map hooked dev=%p", device);
    DWORD o16 = 0;
    if (VirtualProtect(&vt[16], sizeof(void*), PAGE_READWRITE, &o16)) {
        g_orig_CreateRootSig = reinterpret_cast<PFN_CreateRootSignature>(vt[16]);
        vt[16] = reinterpret_cast<void*>(&Hook_CreateRootSignature);
        DWORD junk = 0;
        VirtualProtect(&vt[16], sizeof(void*), o16, &junk);
        log("[rootsig] CreateRootSignature hooked dev=%p", device);
    }
    DWORD o10 = 0;
    if (VirtualProtect(&vt[10], sizeof(void*), PAGE_READWRITE, &o10)) {
        g_orig_CreateGfxPso = reinterpret_cast<PFN_CreateGraphicsPipelineState>(vt[10]);
        vt[10] = reinterpret_cast<void*>(&Hook_CreateGraphicsPipelineState);
        DWORD junk = 0;
        VirtualProtect(&vt[10], sizeof(void*), o10, &junk);
        log("[pso] CreateGraphicsPipelineState hooked dev=%p", device);
    }
    {
        ID3D12Device2* dev2 = nullptr;
        auto* dev0 = static_cast<ID3D12Device*>(device);
        if (SUCCEEDED(dev0->QueryInterface(IID_PPV_ARGS(&dev2))) && dev2) {
            DWORD o47 = 0;
            if (VirtualProtect(&vt[47], sizeof(void*), PAGE_READWRITE, &o47)) {
                g_orig_CreatePso = reinterpret_cast<PFN_CreatePipelineState>(vt[47]);
                vt[47] = reinterpret_cast<void*>(&Hook_CreatePipelineState);
                DWORD junk = 0;
                VirtualProtect(&vt[47], sizeof(void*), o47, &junk);
                log("[pso] ID3D12Device2::CreatePipelineState hooked dev=%p", device);
            }
            dev2->Release();
        }
    }
    DWORD o17 = 0;
    if (VirtualProtect(&vt[17], sizeof(void*), PAGE_READWRITE, &o17)) {
        g_orig_CreateCBV = reinterpret_cast<CreateCBVFn>(vt[17]);
        vt[17] = reinterpret_cast<void*>(&hk_CreateCBV);
        DWORD junk = 0;
        VirtualProtect(&vt[17], sizeof(void*), o17, &junk);
        log("[cbv] CreateConstantBufferView hooked dev=%p orig=%p", device,
            (void*)g_orig_CreateCBV);
    }
    // Real game objects only: no throwaway device through sl.interposer.
    DWORD o20 = 0;
    if (VirtualProtect(&vt[20], sizeof(void*), PAGE_READWRITE, &o20)) {
        g_orig_CreateRTV = reinterpret_cast<CreateRTVFn>(vt[20]);
        vt[20] = reinterpret_cast<void*>(&hk_CreateRTV);
        DWORD junk = 0;
        VirtualProtect(&vt[20], sizeof(void*), o20, &junk);
        g_rtv_hook_installed.store(true, std::memory_order_release);
        log("[mirror] real device CreateRTV hooked dev=%p orig=%p",
            device, (void*)g_orig_CreateRTV);
    }
}

static HRESULT WINAPI Hook_D3D12CreateDevice(
        IUnknown* adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void** out) {
    HRESULT hr = g_orig_D3D12CreateDevice(adapter, fl, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        {
            std::lock_guard<std::mutex> lock(g_game_object_mtx);
            if (!g_game_device) {
                auto* device = reinterpret_cast<ID3D12Device*>(*out);
                device->AddRef();
                g_game_device = device;
                log("[mirror] captured real game device=%p", device);
            }
        }
        patch_device_descriptor_slot(*out);
    }
    return hr;
}

static bool g_desc_ring_probe_installed = false;

// Inject a 2nd (eye) context into a RENDER manager's map, called from the
// GraphContextPrepare hook (its `manager` arg is a real render manager). We clone
// the manager's OWN main context (key-0) view-params at ctx+7712, IPD-shift the
// camera, and create an eye-keyed context via sub_14036FD10 so  if the prepare
// then rebuilds its active list (+0x48) from the map  the FG loop builds the eye.
// Register the eye via the ENGINE'S OWN registration QUEUE (sub_142906A28), the
// same path the mirror/RTT use  this creates a properly set-up context (a5=1 via
// sub_14079AA1C) that GraphContextPrepare then appends to the active list. We build
// an eye request = main ctx's view-params (IPD-shifted camera) + distinct key @944
// + mode 0 (full) @976 + zeroed extras. sub_142906A28 fills mgr+296; the g_orig
// GraphContextPrepare that runs right after consumes it and builds the eye.
// --- RTT-camera view-create resolution override (C2: dynamic resolution) ---
// sub_1404FBAFC(a1=RTT component, a2) creates the offscreen view; it reads the view dims from
// comp+0x258 (width) / comp+0x25C (height). VRCAM bakes 1600x900. We overwrite those with a
// dynamic resolution just before the engine reads them => the engine creates a FULLY-registered
// view at OUR resolution (no cascade crashes). Component identified by render-host vtable RVA
// 0x307BFD0 + the VRCAM's baked 1600x900.
constexpr uintptr_t RTT_VIEWCREATE_RVA = 0x4FBAFC;   // sub_1404FBAFC
constexpr uintptr_t RTT_HOST_VTABLE_RVA = 0x307BFD0; // entRenderToTextureCameraComponent host vtable
using RttViewCreateFn = char (__fastcall*)(__int64, __int64);
static RttViewCreateFn g_orig_rtt_viewcreate = nullptr;
// OFF by default: the VRCAM asset texture is now authored at the target
// resolution (e.g. 2444x2444), and the RT-activation derives the view dims
// from the output texture. Forcing 1222 here SHRANK the render and meant no
// 2444^2 resource ever existed => dump_rt found nothing. Leave the hook
// installed (so we can flip it live via IPC) but pass-through by default.
bool g_rtt_res_override = false;
uint32_t g_rtt_w = 1222;   // only used when g_rtt_res_override is toggled on
uint32_t g_rtt_h = 1222;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugRttHits = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugRttComp = 0;
// Extra debug exports so x64dbg can read the live RTT component + the dims the
// view-create actually receives, regardless of the (default-off) override.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugRttW = 0;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugRttH = 0;

// --- Dynamic RTT resize via engine ResizeDynamicTexture (match MAIN, no assets) ---
// The vrcam RENDER size = the OUTPUT DynamicTexture size (*(comp+0x1E8) ->
// dtex, width@0x40/height@0x44), NOT comp resolutionWidth/Height (that only
// affects view-create dims, the render follows the texture). So resize the dtex
// itself via the engine's thread-marshaled ResizeDynamicTexture (texMgr vtable[80]
// = sub_14291A4D4). texMgr = *(*(exe+0x3427C00)+0x70). Signature proven live:
//   char f(rcx=texMgr, rdx=&dtexPtr, r8d=w, r9d=h, [rsp20]=flag).
// Target = explicit CyberpunkVR_RttResizeW/H, else MAIN's render dims (g_main_ctx
// +0x44 W / +0x4C H). Default OFF (enable live via x64dbg to verify no crash).
constexpr uintptr_t RESIZE_DYNTEX_RVA = 0x291A4D4;   // sub_14291A4D4
using ResizeDynTexFn = char (__fastcall*)(void*, void**, uint32_t, uint32_t, int);
static ResizeDynTexFn g_resize_dyntex = nullptr;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_RttResizeMatchMain = 0; // 1=resize dtex -> target
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_RttResizeW = 0;         // explicit W (0=use main)
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_RttResizeH = 0;         // explicit H (0=use main)
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugRttResizeHits = 0;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugRttDtexW = 0;      // current dtex W (live)
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugRttDtexH = 0;
// The bound VRCAM component. Resolved ONCE (by the selected resolution) and then reused, so
// the per-frame writes never have to re-decide which component they are talking to.
static std::atomic<uintptr_t> g_vrcam_comp{0};
// Its AUTHORED fov, captured at bind before anything of ours writes to it.
static float g_vrcam_base_fov = 0.f;
extern "C" __declspec(dllexport) float    CyberpunkVR_DebugVrcamBaseFov = 0.f;
// Components skipped because their dims are not the selected resolution. Non-zero here is
// normal: it counts the ones we correctly refused to point the fov writes at.
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugRttCompRejects = 0;

static void maybe_resize_rtt(uintptr_t comp);   // defined after g_main_ctx

static __int64 __fastcall Detour_RTTViewCreate(__int64 a1, __int64 a2) {
    if (a1) {
        __try {
            uint64_t vt = *reinterpret_cast<uint64_t*>(a1);
            if (vt == reinterpret_cast<uint64_t>(g_exe_base) + RTT_HOST_VTABLE_RVA) {
                uint32_t w = *reinterpret_cast<uint32_t*>(a1 + 0x258);
                uint32_t h = *reinterpret_cast<uint32_t*>(a1 + 0x25C);
                // Bind ONE component and keep it. This used to latch whichever RTT component
                // came through last, which was harmless when the player carried a single
                // vrcam component -- but there is now one per resolution, so the per-frame
                // fov writes could land on a disabled (or destroyed) one.
                // The selected resolution is the discriminator, and it is consulted ONLY on
                // the first bind: the authored set has exactly one component per resolution,
                // and the resolution comes from the launcher's pick, so nothing is hardcoded.
                const uintptr_t cached = g_vrcam_comp.load(std::memory_order_relaxed);
                const uint32_t sel_w = g_vrcam_sel_w.load(std::memory_order_relaxed);
                const uint32_t sel_h = g_vrcam_sel_h.load(std::memory_order_relaxed);
                const bool dims_match = !sel_w || !sel_h || (w == sel_w && h == sel_h);
                if (cached && static_cast<uintptr_t>(a1) != cached) {
                    if (!dims_match) {
                        ++CyberpunkVR_DebugRttCompRejects;
                        return g_orig_rtt_viewcreate(a1, a2);
                    }
                    // Same selected resolution, different object: the component was destroyed
                    // and re-created (resolution switch / entity respawn). Re-bind, or every
                    // later write would target freed memory.
                    g_vrcam_comp.store(static_cast<uintptr_t>(a1), std::memory_order_release);
                    g_vrcam_base_fov = *reinterpret_cast<float*>(a1 + 0x128);
                    CyberpunkVR_DebugVrcamBaseFov = g_vrcam_base_fov;
                    log("[rtt] re-bound vrcam component %p -> %p (%ux%u)",
                        reinterpret_cast<void*>(cached), reinterpret_cast<void*>(a1), w, h);
                } else if (!cached) {
                    if (!dims_match) {
                        if ((CyberpunkVR_DebugRttCompRejects++ % 600) == 0)
                            log("[rtt] ignoring component %p %ux%u (selected %ux%u)",
                                reinterpret_cast<void*>(a1), w, h, sel_w, sel_h);
                        return g_orig_rtt_viewcreate(a1, a2);
                    }
                    g_vrcam_comp.store(static_cast<uintptr_t>(a1), std::memory_order_release);
                    // Capture the AUTHORED fov before anything of ours writes to it -- the
                    // zoom needs a reference that cannot drift with our own output.
                    g_vrcam_base_fov = *reinterpret_cast<float*>(a1 + 0x128);
                    CyberpunkVR_DebugVrcamBaseFov = g_vrcam_base_fov;
                    log("[rtt] bound vrcam component %p %ux%u fov=%.3f",
                        reinterpret_cast<void*>(a1), w, h, g_vrcam_base_fov);
                }
                CyberpunkVR_DebugRttComp = static_cast<uint64_t>(a1);
                CyberpunkVR_DebugRttW = w;
                CyberpunkVR_DebugRttH = h;
                g_mirror_vrcam_serial.fetch_add(1, std::memory_order_release);
                // Resize the OUTPUT DynamicTexture to the target (main res) so the
                // render follows. Converges over 1-2 view-creates (async render cmd).
                maybe_resize_rtt(static_cast<uintptr_t>(a1));
                if ((CyberpunkVR_DebugRttHits++ % 300) == 0) {
                    log("[rtt] view-create comp=%p dims=%ux%u hits=%llu",
                        reinterpret_cast<void*>(a1), w, h,
                        (unsigned long long)CyberpunkVR_DebugRttHits);
                }
                if (g_rtt_res_override) {
                    *reinterpret_cast<uint32_t*>(a1 + 0x258) = g_rtt_w;
                    *reinterpret_cast<uint32_t*>(a1 + 0x25C) = g_rtt_h;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return g_orig_rtt_viewcreate(a1, a2);
}

// --- DIAGNOSTIC (phase 1): where does the eye go in sub_140219730's per-view loop? ---
// FULL build  = sub_141D43040(v3, v50, v50+112, ctx+6096, v5)  -> a4 = ctx+6096
// incremental = sub_141D475B0(v3, v50, v50+112, ctx+6096, v5)  -> a4 = ctx+6096
// FinalOnly (LABEL_80) = sub_1428E6700(...)  (ctx not in args -> can't tag the eye)
// If FullEye stays 0 while FullTotal climbs => the eye is diverted BEFORE the full
// build (FinalOnly/skip) => it needs the natural view path, not forcing.
constexpr uintptr_t FULL_BUILD_RVA = 0x1D43040;   // sub_141D43040
constexpr uintptr_t INCR_BUILD_RVA = 0x1D475B0;   // sub_141D475B0
using BuildFn = __int64 (__fastcall*)(__int64, __int64, __int64, __int64, __int64);
static BuildFn g_orig_full_build = nullptr;
static BuildFn g_orig_incr_build = nullptr;
bool g_enable_build_probe = true;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugFullEye = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugFullTotal = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugIncrEye = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugIncrTotal = 0;

// ---- FrameGraph feature-flag diff + force for the VRCAM (RTT) view ----------
// a4 = ctx+6096; a4[0]/a4[1] are the two 64-bit feature-flag words read by the
// builder (sub_1407305B0(a4,N) => word[N/64] bit N%64). ctx+0x28 = view key
// (== the virtualCameraName CName hash for the RTT), ctx+0x44/0x48 = w/h.
// We log every distinct view's flags, capture the richest 16:9 view as the
// "main template", and (when g_rtt_force_flags is toggled on via IPC) OR that
// template into the VRCAM view's flags so the builder emits the SAME passes
// (tonemap / final) that main gets. Guarded so a bad pass can't kill the game.
// VRCAM view key lives in g_vrcam_ctx_key (top of file): it is derived from the selected
// component's virtualCameraName, because there is one component per render resolution.
// Force ON by default: the VRCAM full build is ONE-SHOT + cached, and it runs
// early (before any UI toggle), so the OR must already be armed when that single
// build executes. Seed the main-template with the known observed main flags so
// the OR works even before a main-ish view is captured this session (the live
// popcount heuristic overwrites these once the real desktop view is seen).
std::atomic<bool> g_rtt_force_flags{true};
// Force VRCAM's feature flags to main's for full quality, but additionally SET
// feature-bit 50 = "reuse shadow cascades" (fg builder sub_141D43040: if
// sub_1407305B0(flags,50) -> SKIP ClearShadowCascades + RenderCascade%u and
// reference the existing atlas). main has bit50 CLEAR (it renders cascades);
// giving VRCAM bit50 makes it SAMPLE main's cascades instead of regenerating
// them into the shared atlas -> no main shadow flicker, VRCAM keeps quality.
std::atomic<bool> g_force_view_flags{true};
static const uint64_t SHADOW_CASCADE_REUSE_BIT = (1ULL << 50);  // SET = skip cascade regen
// Distant shadows: work-fn sub_140373998 renders them only if flags bit 11
// (0x800) is set. VRCAM naturally lacks it; forcing flags=main gave it bit11 ->
// VRCAM regenerated distant shadows into the shared distant-shadow buffer ->
// out-the-window flicker on main. CLEAR it so VRCAM reuses main's distant shadows.
static const uint64_t DISTANT_SHADOW_BIT = (1ULL << 11);         // CLEAR = skip distant regen
// f1 bit 24 (overall bit 88) is the master gate for the async lighting-compute
// block (GI / clustered light grid / reflections). Builder: v19 = f1 & 0x1000000
// gates v21..v26 -> v273 -> AsyncComputeDuringShadowmaps. VRCAM rebuilding those
// view-dependent global structures collides with main -> light/shadow flicker on
// distant (out-the-window) geometry. CLEAR it so VRCAM reuses main's.
static const uint64_t LIGHTING_COMPUTE_BIT_F1 = (1ULL << 24);    // CLEAR in f1 = reuse main's GI/clusters
// BISECTION: main-has / vrcam-naturally-lacks f0 bits are the suspects that make
// vrcam rebuild view-dependent global lighting structures. Clearing this group
// from the forced flags tests whether the out-the-window light/shadow collision
// lives in the HIGH half {26,31,32,33,34,58}. bits: 26=0x4000000 31=0x80000000
// 32=0x1_00000000 33=0x2_00000000 34=0x4_00000000 58=0x400_00000000_0000.
// bit 31 = Global Illumination. Confirmed: CRenderNode_GlobalIllumination::work
// (sub_14077E664) does `if (sub_14023AF5C(a2, 31)) { update GI (sub_14077F758) }`.
// main has it (builds GI); VRCAM forcing it -> rebuilds the shared GI buffer from
// its frustum -> main GI (ambient light/shadows) flicker out-the-window. CLEARED
// for VRCAM -> its GI node skips the update and reuses main's GI.
static const uint64_t GI_FEATURE_BIT = (1ULL << 31);
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugFgMainF0 = 0x3C00017FAD75FF51ULL;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugFgMainF1 = 0x000000000517F008ULL;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugFgRttF0 = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugFgRttF1 = 0;
//  FRAME-GRAPH UPSCALER SELECTOR (ROOT crop fix) 
// SCENE_FULL (sub_141D43040) picks the temporal upscaler at BUILD time via
// sub_1407305B0(a4,N) = bit(N&0x3F) of a4[N>>6]. Groups 69/71/72/73 = DLSS/FSR2/FSR3/
// XeSS. If NONE are set it emits the NATIVE TAAU/resolve node (sub_1418629D4 ->
// PipelineState_563) that re-does a temporal upscale at render-res -> CROPS the DLSS
// output (1418 viewport on the 2444 target). main has group 69 (DLSS) SET at build ->
// no 563. vrcam does NOT (its native upscaler mode is TAA) -> 563. Our runtime 0x45
// force (in ApplyDLSS) is TOO LATE -- the graph already baked the TAAU branch.
// FIX: set group 69 (bit5, == flag 0x45) and clear 71/72/73 in the BUILD flag bitset
// for vrcam, at flag-compute AND right before the full builder. Gated on VrcamDlss
// ONLY (not VrcamDlssScale): group69 must be set even for DLAA else the builder thinks
// there's no temporal upscaler. Only forced when MAIN itself selected DLSS.
constexpr uint64_t FG_DLSS_BIT_F1      = 1ull << (69 - 64);   // 0x020 (group 69 / DLSS)
constexpr uint64_t FG_UPSCALER_MASK_F1 =                      // 0x3A0
    (1ull << (69 - 64)) | (1ull << (71 - 64)) | (1ull << (72 - 64)) | (1ull << (73 - 64));
static std::atomic<uint64_t> g_main_upscaler_groups{0};       // MAIN's chosen upscaler groups (f1 & mask)
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMainUpscalerGroups = 0; // diag: main's upscaler bits
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugUpscalerForceHits  = 0; // diag: vrcam forced -> DLSS count
// fwd (real definitions live further below near the DLSS-for-vrcam block)
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_VrcamDlss;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_VrcamDlssScale;
// LIVE A/B of the two reuse bits that are the ONLY difference between vrcam and
// main flags (camera + env handles proven identical). Diagnoses which reuse bit
// breaks vrcam lighting (wrong light / triangle shadows / light bleed / no refl).
//   0 = current: cascade-reuse (bit50 SET) + GI-reuse (bit31 CLEAR)
//   1 = EXACT main flags (bit50 CLEAR, bit31 SET) -> vrcam builds own cascades+GI
//       (expect main shadow flicker, but tells us if reuse is what breaks visuals)
//   2 = cascade-reuse only (bit50 SET, bit31 SET=GI native/own)
//   3 = GI-reuse only (bit50 CLEAR=cascade native/own, bit31 CLEAR)
// DEFAULT 1: with the temporal-history fix (StreamlineHistoryFix) giving vrcam a
// real per-view temporal view-state, vrcam building its OWN cascades+GI no longer
// collides with main (main flicker GONE, proven live) and fixes interior shadows/
// sun light that cascade/GI-reuse (mode 0) broke. mode 0 kept for A/B fallback.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_VrcamFlagMode = 1;
// VRCAM final-color EXTRACTION fix: the full scene builder gates ExtractionSceneColor
// (sub_1428E5748) and ExtractionFinalColor (sub_140982B5C) on build-bit 64
// (sub_1407305B0(a4,64) == f1 bit 0). CopyToTexture (unconditional) READS the final-
// color 0x3D7E6258 -> for main it's written by a separate final builder, but VRCAM
// only runs the scene builder where bit 64 is 0 -> extraction skipped -> VRCAM copies
// an unwritten (black) final-color. Set bit 64 in VRCAM's computed flags so the scene
// builder emits the extraction passes for VRCAM too. Default OFF -> toggle live.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_VrcamExtractionFix = 1;   // proven live-safe: adds ExtractionFinalColor
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugVrcamExtractionHits = 0;
// VRCAM final-color GROUP: the scene builder only adds ExtractionFinalColor (bit 64),
// NOT ClearFinalColorTarget / DeclareCommonResourceAllocs_FinalOnly (those live only in
// the separate blank/final builder that VRCAM never runs). Hook the Extraction ADDER
// (sub_140982B5C) and, when it is called from the SCENE builder (== VRCAM, since only
// VRCAM has bit 64 set there), inject the Declare + Clear adders FIRST so the graph gets
// Declare -> Clear -> Extraction, matching the engine's blank-builder order. Return-addr
// range-gated to the full/incremental scene builders so MAIN's blank builder (which
// already adds all three) is never double-fed. Default OFF (Declare_FinalOnly may
// re-declare final-color) -> enable + verify live.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_VrcamFinalGroup = 0;         // OFF: ClearFinalColorTarget literally clears 0x3D7E6258 (the RenderFinal2D output we capture) to the bg color -> wipes the vrcam frame to black. RenderFinal2D already draws the full frame; no clear needed.
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugVrcamFinalGroupHits = 0;
// VRCAM composition GROUP: DrawComposition / CompositionPostProcess / FullscreenVideo
// live in SCENE_FULL under build-bit 82 and in the final builder sub_140982C7C; VRCAM
// runs SCENE_INCR (sub_141D475B0) which has NEITHER -> missing. RenderFinal2D is added
// by name. Inject all of them via the same Extraction-adder hook (fires for VRCAM with
// the builder ctx). NOTE: CopyToTexture copies final-color 0x3D7E6258 (pre-composition);
// composition writes 0x31CF52F9 -> adding it does NOT change what the mirror copies. It
// makes VRCAM's graph node-complete. Default OFF -> enable + verify live.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_VrcamCompositionGroup = 0;   // default OFF: composition inputs absent in VRCAM RTT graph -> downstream fault
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugVrcamCompositionHits = 0;
constexpr uintptr_t EXTRACTION_ADDER_RVA   = 0x982B5C;   // sub_140982B5C (ExtractionFinalColor)
constexpr uintptr_t DECLARE_ADDER_RVA      = 0x982974;   // sub_140982974 (DeclareCommonResourceAllocs_FinalOnly)
constexpr uintptr_t CLEAR_ADDER_RVA        = 0x982A40;   // sub_140982A40 (ClearFinalColorTarget)
constexpr uintptr_t DRAWCOMP_ADDER_RVA     = 0x982AA0;   // sub_140982AA0 (DrawComposition)
constexpr uintptr_t COMPOSITION_ADDER_RVA  = 0x982B00;   // sub_140982B00 (CompositionPostProcess)
constexpr uintptr_t FSVIDEO_ADDER_RVA      = 0x986B30;   // sub_140986B30 (FullscreenVideo)
constexpr uintptr_t ADD_NAMED_PASS_RVA     = 0x9853B4;   // sub_1409853B4 (add pass by name)
constexpr uintptr_t SCENE_FULL_LO_RVA = 0x1D43040, SCENE_FULL_HI_RVA = 0x1D43040 + 0x456F;
constexpr uintptr_t SCENE_INCR_LO_RVA = 0x1D475B0, SCENE_INCR_HI_RVA = 0x1D475B0 + 0xA33;
// VRCAM actually builds via the RTT builder sub_141D47FF0 (live: FullEye/IncrEye=0 yet
// Extraction present). MAIN never enters it (main uses SCENE_FULL + final sub_140982C7C),
// so injecting when the Extraction adder is called from here is VRCAM-only.
constexpr uintptr_t SCENE_RTT_LO_RVA  = 0x1D47FF0, SCENE_RTT_HI_RVA  = 0x1D47FF0 + 0x12E7;
using PassAdderFn = __int64 (__fastcall*)(__int64, __int64, __int64, int);
using NamedPassFn = __int64 (__fastcall*)(unsigned int, __int64, __int64, __int64, const char*, int);
static PassAdderFn g_orig_extraction_adder = nullptr;
static PassAdderFn g_declare_adder = nullptr;
static PassAdderFn g_clear_adder = nullptr;
static PassAdderFn g_drawcomp_adder = nullptr;
static PassAdderFn g_composition_adder = nullptr;
static PassAdderFn g_fsvideo_adder = nullptr;
static NamedPassFn g_add_named_pass = nullptr;

static int      g_fg_main_pop = 44;   // popcount of the seeded default main flags
static uint64_t g_fg_logged[32];
static int      g_fg_logged_n = 0;
static int fg_popcount(uint64_t x) { int c = 0; while (x) { x &= x - 1; ++c; } return c; }
static bool g_fg_vrcam_full_logged = false;
static bool g_fg_vrcam_incr_logged = false;
// Defined with the HUD identification state it resets, far below. See the call site.
static void hud_rearm_for_new_graph(uint64_t key);
static void fg_observe(__int64 a4, const char* which) {
    if (!a4) return;
    __try {
        uint8_t* ctx = reinterpret_cast<uint8_t*>(a4) - 6096;
        uint64_t key = *reinterpret_cast<uint64_t*>(ctx + 0x28);
        uint32_t w   = *reinterpret_cast<uint32_t*>(ctx + 0x44);
        uint32_t h   = *reinterpret_cast<uint32_t*>(ctx + 0x48);
        uint64_t f0  = *reinterpret_cast<uint64_t*>(a4);
        uint64_t f1  = *reinterpret_cast<uint64_t*>(a4 + 8);
        bool seen = false;
        for (int i = 0; i < g_fg_logged_n; ++i) if (g_fg_logged[i] == key) { seen = true; break; }
        if (!seen && g_fg_logged_n < 32) {
            g_fg_logged[g_fg_logged_n++] = key;
            log("[fgflags] %s key=%016llX %ux%u f0=%016llX f1=%016llX%s",
                which, (unsigned long long)key, w, h,
                (unsigned long long)f0, (unsigned long long)f1,
                key == g_vrcam_ctx_key ? " <-VRCAM" : "");
            // A FULL BUILD UNDER A KEY WE HAVE NEVER SEEN IS A NEW GRAPH, AND OUR HUD
            // IDENTIFICATION DOES NOT SURVIVE ONE.
            //
            // Opening the map or the inventory rebuilds the frame graph, and it does not come back
            // the way it left. A tester's session: key 0000000000000000 at 2560x2560 for twenty
            // minutes, then D512F33B8A4F15C9 at 1280x1280 when the map opened, then
            // 9947B0B7A7CD0843 at 2848x2848 on the way back -- a third key at a third size. One
            // line before that first rebuild the composite lost all five of its inputs at once
            // ("waiting on: surface blur-pyramid exposure frame-constants composite-constants")
            // and never regained them, because identification had latched onto a node from the old
            // graph and switched descriptor matching off behind itself. Nothing left to find it
            // with. Changing a graphics setting brought it back, which is the same thing from the
            // other direction: yet another rebuild, and that one happened to re-name the node.
            //
        }
        // RE-ARM ON EVERY TRANSITION, NOT ON FIRST SIGHTING.
        //
        // The log above only prints keys it has never seen, and the first version of this hung the
        // re-arm off that -- which would have fixed the first map opening of a session and no
        // other, while the report is explicitly that it keeps happening. What matters is the
        // CHANGE: the full builder ran under a different key than last time, so whatever the HUD
        // path is holding belongs to the previous one.
        //
        // Debounced, because two graphs alternating frame to frame would otherwise re-arm forever
        // and never let an identification settle.
        if (which[0] == 'f') {
            static uint64_t s_lastFullKey = 0;
            static bool     s_haveLastFullKey = false;
            static uint64_t s_lastRearmMs = 0;
            if (!s_haveLastFullKey) {
                s_haveLastFullKey = true;
                s_lastFullKey = key;
            } else if (key != s_lastFullKey) {
                s_lastFullKey = key;
                const uint64_t nowMs = GetTickCount64();
                if (nowMs - s_lastRearmMs >= 250) {
                    s_lastRearmMs = nowMs;
                    hud_rearm_for_new_graph(key);
                }
            }
        }
        // Which builder processes the VRCAM view? (full has tonemap/EndRender;
        // if VRCAM only ever shows up via incr/FinalOnly it never tonemaps.)
        if (key == g_vrcam_ctx_key) {
            if (which[0] == 'f' && !g_fg_vrcam_full_logged) {
                g_fg_vrcam_full_logged = true;
                log("[fgflags] VRCAM built via FULL (sub_141D43040) f0=%016llX f1=%016llX",
                    (unsigned long long)f0, (unsigned long long)f1);
            } else if (which[0] == 'i' && !g_fg_vrcam_incr_logged) {
                g_fg_vrcam_incr_logged = true;
                log("[fgflags] VRCAM built via INCR (sub_141D475B0) f0=%016llX f1=%016llX",
                    (unsigned long long)f0, (unsigned long long)f1);
            }
        }
        // Earliest point we hold the second view's ctx: grant the HUD capability here, before
        // the graph is built and before anything downstream reads it.
        if (key == g_vrcam_ctx_key) hud_grant_capability(reinterpret_cast<uintptr_t>(ctx));
        if (key == g_vrcam_ctx_key) {
            if (g_rtt_force_flags.load(std::memory_order_relaxed) &&
                (CyberpunkVR_DebugFgMainF0 | CyberpunkVR_DebugFgMainF1)) {
                uint64_t n0 = f0 | CyberpunkVR_DebugFgMainF0;
                uint64_t n1 = f1 | CyberpunkVR_DebugFgMainF1;
                *reinterpret_cast<uint64_t*>(a4)     = n0;   // actually applied
                *reinterpret_cast<uint64_t*>(a4 + 8) = n1;
                CyberpunkVR_DebugFgRttF0 = n0;               // export the POST-OR value
                CyberpunkVR_DebugFgRttF1 = n1;
                static uint64_t s_forced0 = 0;
                if (s_forced0 != n0) {                        // log once per change
                    s_forced0 = n0;
                    log("[fgflags] FORCED vrcam -> f0=%016llX f1=%016llX (was %016llX/%016llX)",
                        (unsigned long long)n0, (unsigned long long)n1,
                        (unsigned long long)f0, (unsigned long long)f1);
                }
            } else {
                CyberpunkVR_DebugFgRttF0 = f0;
                CyberpunkVR_DebugFgRttF1 = f1;
            }
            // UPSCALER SELECTOR (guarantee, runs right before the FULL builder reads a4
            // for `if(!69 && !71 && !72 && !73)`): force group 69 (DLSS), clear 71/72/73.
            // See FG_UPSCALER block. Gated on VrcamDlss; only when MAIN chose DLSS.
            if (CyberpunkVR_VrcamDlss &&
                (g_main_upscaler_groups.load(std::memory_order_acquire) & FG_DLSS_BIT_F1)) {
                uint64_t* fr = reinterpret_cast<uint64_t*>(a4);
                uint64_t nf1 = (fr[1] & ~FG_UPSCALER_MASK_F1) | FG_DLSS_BIT_F1;
                if (nf1 != fr[1]) {
                    fr[1] = nf1;
                    CyberpunkVR_DebugFgRttF1 = nf1;
                    ++CyberpunkVR_DebugUpscalerForceHits;
                }
            }
        } else if (w >= 1280 && w >= h) {          // main-ish 16:9 view
            int pop = fg_popcount(f0) + fg_popcount(f1);
            if (pop > g_fg_main_pop) {
                g_fg_main_pop = pop;
                CyberpunkVR_DebugFgMainF0 = f0;
                CyberpunkVR_DebugFgMainF1 = f1;
            }
            // Capture MAIN's chosen upscaler groups (reliable primary is FlagCompute
            // key==0; this main-ish path is a backup). Only latch when DLSS is present
            // so a transient pre-DLSS frame can't clear it.
            if ((f1 & FG_DLSS_BIT_F1) != 0) {
                g_main_upscaler_groups.store(f1 & FG_UPSCALER_MASK_F1, std::memory_order_release);
                CyberpunkVR_DebugMainUpscalerGroups = f1 & FG_UPSCALER_MASK_F1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Attribute a graph-build duration (ticks) to main vs vrcam via the view ctx.
static void prof_add_build(__int64 a4, int64_t dt) {
    bool vrcam = false;
    if (a4) {
        __try {
            uint8_t* ctx = reinterpret_cast<uint8_t*>(a4) - 6096;
            vrcam = (*reinterpret_cast<uint64_t*>(ctx + 0x28) == g_vrcam_ctx_key);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (vrcam) {
        g_prof_build_vrcam_ticks.fetch_add(dt, std::memory_order_relaxed);
        g_prof_build_vrcam_calls.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_prof_build_main_ticks.fetch_add(dt, std::memory_order_relaxed);
        g_prof_build_main_calls.fetch_add(1, std::memory_order_relaxed);
    }
}

static __int64 __fastcall Detour_FullBuild(__int64 a1, __int64 a2, __int64 a3,
                                           __int64 a4, __int64 a5) {
    CyberpunkVR_DebugFullTotal++;
    fg_observe(a4, "full");   // DLSS upscaler-group capture/force backup for the crop fix
    if (CyberpunkVR_ProfEnable) {
        const int64_t t0 = prof_now();
        const __int64 r = g_orig_full_build(a1, a2, a3, a4, a5);
        prof_add_build(a4, prof_now() - t0);
        return r;
    }
    return g_orig_full_build(a1, a2, a3, a4, a5);
}

static __int64 __fastcall Detour_IncrBuild(__int64 a1, __int64 a2, __int64 a3,
                                           __int64 a4, __int64 a5) {
    CyberpunkVR_DebugIncrTotal++;
    fg_observe(a4, "incr");   // DLSS upscaler-group capture/force backup for the crop fix
    if (CyberpunkVR_ProfEnable) {
        const int64_t t0 = prof_now();
        const __int64 r = g_orig_incr_build(a1, a2, a3, a4, a5);
        prof_add_build(a4, prof_now() - t0);
        return r;
    }
    return g_orig_incr_build(a1, a2, a3, a4, a5);
}

// --- per-frame feature-flag WRITER hook (sub_141D49540) ----------------------
// The view's render-feature flags (viewobj+0x17D0 == ctx+6096) are RE-DERIVED
// EVERY frame per-view by sub_141D49540, which returns a pointer to the 16-byte
// flag block that its caller then copies into viewobj+0x17D0:
//     call sub_141D49540 ; movups xmm0,[rax] ; movdqu [rbx+17D0h],xmm0
// This per-frame rewrite is exactly what wiped the flags we forced at the (cached)
// one-shot build. Hook it and, for the VRCAM view (key @ viewobj+0x28), OR the
// captured main-template flags into the freshly computed result on EVERY call ->
// the forced flags now persist through both the build and per-frame node execution
// (the tonemap/bloom/exposure work-fns read viewobj+0x17D0 at record time). If
// these flags gate the reduced passes, they now light up; if not, this proves it.
// a1=renderer a2=outFlags a3=view context a4=view-state; return=outFlags.
constexpr uintptr_t FLAG_COMPUTE_RVA = 0x1D49540;   // sub_141D49540
using FlagComputeFn = __int64(__fastcall*)(void*, __int64, __int64, __int64);
static FlagComputeFn g_orig_flag_compute = nullptr;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugRttFlagForceHits = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugRttEnvBindHits = 0;
// LIVE A/B control for distant-shadow reuse (write via x64dbg to isolate distant
// in VRCAM):  0 = distant OFF for vrcam (bit 11 cleared -> node no-ops, no distant
// shadows).  1 = distant REUSE (bit 11 kept SET + vrcam's distant work skipped ->
// vrcam samples main's distant map).  Flip 1->0->1 and watch vrcam's far-field
// (out-the-window) sun shadows disappear/reappear = proof distant reuse is live.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DistantReuseMode = 1;
// Force VRCAM camera to match MAIN instead of relying on the RTT-camera asset.
// Camera params live at ctx+0x70 (a3-view-params). We copy PROJECTION from main
// (lens/fov/aspect/near/far at a3+0x10..0x48 = ctx+0x80..0xB8, and the inv/fwd
// proj matrices at a3+0x150..0x1D0 = ctx+0x1C0..0x240) but KEEP vrcam's own view
// matrix/position (a3+0x50..0x140 = ctx+0xC0..0x1B0) so steer/IPD still apply.
// Resolution: force vrcam render dims (ctx+0x44/0x48) + rect to main's W/H.
// (LOD/culling in this engine is screen-space-error driven -> forcing resolution
// + fov to main makes VRCAM's LOD selection match main automatically.)
// NOTE: ForceVrcamRes via ctx dims is INVALID  the RTT view is created with dims
// == its RTT texture (2444^2); forcing ctx+0x44/0x48 to a different W/H makes the
// view fail validation at init -> VRCAM never renders (absent from Nsight). To
// change VRCAM resolution, resize the RTT TEXTURE (dynamicTextureRes asset) so the
// dims derive correctly. ForceVrcamCam copies MAIN's camera SCALARS (fov/zoom/near/
// far) into the VRCAM view context so both eyes use matching gameplay camera scalars.
// Live-confirmed ctx layout: +0x90 fovV, +0x9C zoom, +0xB0 nearZ, +0xB4 farZ. It does
// NOT touch orientation (+0x80..0x8C) or aspect (+0x98, vrcam
// keeps its own for its square RTT dims) and copies NO proj matrix. Default ON.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_ForceVrcamCam = 1;   // vrcam fov/zoom/near/far = main
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_ForceVrcamRes = 0;   // DISABLED (breaks RTT view)
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugForceCamHits = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugForceResHits = 0;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugMainW = 0;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugMainH = 0;

// Engine handle copy-assign: sub_1407CDAE4(dst, src) -> AddRef(src), Release(old
// dst), dst = src. This is how the per-view setup (sub_14036F7D4) binds the world
// environment handle into MAIN's view ctx. RTT views never get it (fields stay 0)
// -> no exposure/tonemap/bloom. Using the engine's own refcounted assign (not a
// raw pointer copy) keeps the env alive -> no use-after-free / -1 deref crash.
constexpr uintptr_t HANDLE_ASSIGN_RVA = 0x28DAE4;   // sub_1407CDAE4(dst, src)
using HandleAssignFn = void*(__fastcall*)(void* dst, void* src);
static HandleAssignFn g_handle_assign = nullptr;

// Live MAIN view ctx (key==0): source of the environment handles to mirror.
static uintptr_t g_main_ctx = 0;
// Which named render-mask categories does each view actually hold? One line, both views,
// every category -- so the next "the second eye is missing X" question is answered by reading
// the log instead of by another session of bisecting nodes.
static void render_mask_report() {
    static uint64_t s_last = 0;
    const uint64_t now = GetTickCount64();
    if (s_last && now - s_last < 20000) return;
    const uintptr_t mc = g_main_ctx;
    const uintptr_t vc = g_vrcam_ctx_seen.load(std::memory_order_acquire);
    if (!mc || !vc || !g_exe_base) return;
    s_last = now;
    const uintptr_t base = reinterpret_cast<uintptr_t>(g_exe_base);
    char line[1400];
    int used = 0;
    line[0] = 0;
    for (uint32_t k = 0; k < kRenderMaskCount; ++k) {
        bool m = true, v = true;
        __try {
            const uint64_t* need =
                reinterpret_cast<const uint64_t*>(base + kRenderMasks[k].desc_rva) + 1;
            const uint64_t* hm = reinterpret_cast<const uint64_t*>(mc + 6304);
            const uint64_t* hv = reinterpret_cast<const uint64_t*>(vc + 6304);
            for (int i = 0; i < 32; ++i) {
                if ((hm[i] & need[i]) != need[i]) m = false;
                if ((hv[i] & need[i]) != need[i]) v = false;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        if (used < static_cast<int>(sizeof(line)) - 48)
            used += snprintf(line + used, sizeof(line) - used, "%s=%c%c ",
                             kRenderMasks[k].name, m ? 'M' : '-', v ? 'V' : '-');
    }
    log("[rmask] per-view render-mask categories (M = main has it, V = vrcam has it): %s", line);
}

// The 3 environment handle slots (16 bytes each) that MAIN fills and VRCAM leaves
// zero (found by live diff). Assigned via the engine's refcounted handle-assign.
static const uint32_t kEnvHandleOffs[] = { 0x16A8, 0x1D28, 0x21A8 };

// Additional environment slots, for the scanner's colour grade.
//
// `base\gameplay\focus_mode.envparam` is what tints the screen green: its
// renderAreaSettings/areaParameters[0]/Data carries
//     ldrLut = base\weather\24h_basic\luts\cp2077_scanning_v0001.xbm
//     hdrLut = base\weather\24h_basic\luts\hdri\cp2077_scanning_hdr_acess_v0001.xbm
// (a mod that blanks exactly those two paths removes the tint, which is how this was pinned).
// So the tint is an ENVIRONMENT AREA OVERRIDE pushed onto the player's view, not a render flag
// and not a shader parameter -- which is why every gate, mask, feature bit and constant checked
// so far came back identical between the views.
//
// The three slots above were found as MAIN-set/VRCAM-zero. The diff only ever printed that case,
// so slots where the second view holds its OWN different handle were invisible until
// CyberpunkVR_ViewDataDiff=2. With all differing ranges printed, the graph context shows nine
// more 8-byte fields laid out as three elements of stride 0x3A0 with three pointers each --
// exactly the shape of a blended area-params list holding hdrLut/ldrLut and friends.
//
// One bit per offset so they can be bisected live. Default 0: these are refcounted handles and a
// wrong guess here can take the process down, so they get turned on deliberately, not by default.
static const uint32_t kEnvExtraOffs[] = {
    0x1F0, 0x220, 0x380,      // element 0
    0x590, 0x5C0, 0x720,      // element 1  (+0x3A0)
    0x930, 0x960, 0xAC0,      // element 2  (+0x740)
};
static const uint32_t kEnvExtraCount =
    static_cast<uint32_t>(sizeof(kEnvExtraOffs) / sizeof(kEnvExtraOffs[0]));
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_EnvExtraMask = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugEnvExtraBinds = 0;

// Resolve the render texture-manager: texMgr = *(*(exe+0x3427C00)+0x70).
static void* resolve_texmgr() {
    __try {
        uintptr_t renderer = *reinterpret_cast<uintptr_t*>(g_exe_base + RVA_RENDERER_GLOBAL);
        if (!renderer) return nullptr;
        return *reinterpret_cast<void**>(renderer + 0x70);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Resize the RTT output DynamicTexture (*(comp+0x1E8)) to the target so the vrcam
// render matches. Target = explicit RttResizeW/H, else MAIN dims (g_main_ctx+0x44/
// +0x4C). No-op when already correct. Marshaled/thread-safe. SEH-guarded.
static void maybe_resize_rtt(uintptr_t comp) {
    if (!CyberpunkVR_RttResizeMatchMain || !comp) return;
    __try {
        void* dtex = *reinterpret_cast<void**>(comp + 0x1E8);
        if (!dtex) return;
        uint8_t* d = reinterpret_cast<uint8_t*>(dtex);
        uint32_t cw = *reinterpret_cast<uint32_t*>(d + 0x40);
        uint32_t ch = *reinterpret_cast<uint32_t*>(d + 0x44);
        CyberpunkVR_DebugRttDtexW = cw;
        CyberpunkVR_DebugRttDtexH = ch;
        uint32_t tw = CyberpunkVR_RttResizeW, th = CyberpunkVR_RttResizeH;
        if (!tw || !th) {                 // no explicit target -> match MAIN
            if (!g_main_ctx) return;
            tw = *reinterpret_cast<uint32_t*>(g_main_ctx + 0x44);
            th = *reinterpret_cast<uint32_t*>(g_main_ctx + 0x4C);
        }
        if (!tw || !th || tw > 8192 || th > 8192) return;
        if (cw == tw && ch == th) return; // already correct
        if (!g_resize_dyntex)
            g_resize_dyntex = reinterpret_cast<ResizeDynTexFn>(g_exe_base + RESIZE_DYNTEX_RVA);
        void* texMgr = resolve_texmgr();
        if (!texMgr) return;
        void* holder = dtex;              // proven-safe pattern: &localHolder
        g_resize_dyntex(texMgr, &holder, tw, th, 1);
        // Align the OUTPUT texture's own CPU dims + disable scaleToViewport so the
        // FINAL output resource follows the target too (not just the render RTs).
        *reinterpret_cast<uint32_t*>(d + 0x40) = tw;   // width
        *reinterpret_cast<uint32_t*>(d + 0x44) = th;   // height
        *reinterpret_cast<uint32_t*>(d + 0x48) = 0;    // scaleToViewport off
        ++CyberpunkVR_DebugRttResizeHits;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Bridges the FlagCompute hook to the RectCompute hook within a single per-view
// setup pass (both run sequentially on the same thread inside sub_1404E4xxx).
static thread_local bool     t_vrcam_setup = false;
static thread_local uint32_t t_vrcam_w = 0;
static thread_local uint32_t t_vrcam_h = 0;
// Same numbers, readable from the RECORDING threads. t_vrcam_w/h are thread_local and set on the
// thread that runs FlagCompute, so a command-list hook cannot use them.
static std::atomic<uint32_t> g_vrcam_view_w{0};
static std::atomic<uint32_t> g_vrcam_view_h{0};

// STEP 1 SCOPE: this file forces the VRCAM view's PROJECTION only -- fov, zoom, near
// and far, copied from MAIN so the second view frames the world identically. It does
// NOT touch camera position or orientation. Every eye-offset / tripod / mirror / HMD
// path that used to live here is gone: writing the camera at this stage happens after
// culling and after the weapon viewmodel is placed, so it slid the world and dragged
// the weapon with the head. Camera work belongs in the engine's own camera hooks and
// is a later step.
extern "C" __declspec(dllexport) int32_t CyberpunkVR_OverlayVisible = 1;

// Captured from MAIN's camera ctx (slot 0) and applied to VRCAM (slot 1) in the same
// per-frame camera writer to keep the second view's camera scalars and culling in sync.
static float g_main_cam_fov  = 0.f;
static float g_main_cam_zoom = 0.f;
static float g_main_cam_near = 0.f;
static float g_main_cam_far  = 0.f;
// MAIN's forward-projection vertical scale (ctx+0x214 = cot(fovV/2)) -- the field that
// carries the effective world ADS magnification and the source the VRCAM FOV is derived from.
static float g_main_proj_yy = 0.f;
static float g_main_ads_zoom_factor = 1.0f;   // MAIN/world ADS scale; 1.0 = no zoom
extern "C" __declspec(dllexport) float CyberpunkVR_DebugMainCamFov      = 0.f;
extern "C" __declspec(dllexport) float CyberpunkVR_DebugMainProjYY      = 0.f;
// Live MAIN/world ADS magnification. This production value keeps overlay projection and the
// first-person weapon/body projection on the same angular scale; it is not VRCAM telemetry.
extern "C" __declspec(dllexport) float CyberpunkVR_MainAdsZoomFactor     = 0.f;
extern "C" __declspec(dllexport) float CyberpunkVR_DebugVrcamWantFov    = 0.f;
// Explicit vertical FOV in degrees for the VRCAM eye. 0 = follow MAIN, which is what the
// flat-screen testbed wants. This is the hook for the headset: once the HMD drives the eye,
// put its FOV here and MAIN's ADS magnification keeps applying on top of it.
extern "C" __declspec(dllexport) float CyberpunkVR_VrcamFovDeg = 0.f;
// Last fov/zoom we forced a vrcam view-rebuild for -> only re-dirty on an actual change
// (avoids a per-frame RTT view rebuild when fov/zoom are stable; the game already
// rebuilds on camera movement).
static float g_last_forced_fov  = -1.f;
static float g_last_forced_zoom = -1.f;

static __int64 __fastcall Detour_FlagCompute(void* a1, __int64 a2, __int64 a3, __int64 a4) {
    t_vrcam_setup = false;
    bool vrcam = false;
    if (a3 && g_rtt_force_flags.load(std::memory_order_relaxed)) {
        __try {
            uint64_t key = *reinterpret_cast<uint64_t*>(a3 + 0x28);
            if (key == 0) {
                g_main_ctx = static_cast<uintptr_t>(a3);   // live MAIN env source
                // IPD stereo is applied in Detour_SlConstants (the camera writer,
                // the struct the render actually reads)  NOT this FlagCompute ctx.
            } else if (key == g_vrcam_ctx_key) {
                vrcam = true;
                uint32_t w = *reinterpret_cast<uint32_t*>(a3 + 0x44);
                uint32_t h = *reinterpret_cast<uint32_t*>(a3 + 0x4C);  // H@+0x4C ([W,W,H,H]); +0x48 is a W-dup (rect square bug)
                // FORCE resolution = MAIN: resize VRCAM render dims to main's W/H
                // (main dims @ g_main_ctx+0x44/0x48). Rect below then uses main size.
                if (g_main_ctx && CyberpunkVR_ForceVrcamRes) {
                    uint32_t mw = *reinterpret_cast<uint32_t*>(g_main_ctx + 0x44);
                    uint32_t mh = *reinterpret_cast<uint32_t*>(g_main_ctx + 0x48);
                    CyberpunkVR_DebugMainW = mw; CyberpunkVR_DebugMainH = mh;
                    if (mw && mh) {
                        w = mw; h = mh;
                        *reinterpret_cast<uint32_t*>(a3 + 0x44) = mw;
                        *reinterpret_cast<uint32_t*>(a3 + 0x48) = mh;
                        ++CyberpunkVR_DebugForceResHits;
                    }
                }
                if (w && h) {
                    // Give FlagCompute a valid render rect BEFORE it runs: it reads
                    // ctx+0x14 (sub_1401E4B60) to decide the lighting feature set.
                    // Empty rect -> reduced flags -> no lighting-composite resources.
                    *reinterpret_cast<uint32_t*>(a3 + 0x14) = 0;
                    *reinterpret_cast<uint32_t*>(a3 + 0x18) = 0;
                    *reinterpret_cast<uint32_t*>(a3 + 0x1C) = w;
                    *reinterpret_cast<uint32_t*>(a3 + 0x20) = h;
                    t_vrcam_setup = true;
                    t_vrcam_w = w;
                    t_vrcam_h = h;
                    g_vrcam_view_w.store(w, std::memory_order_release);
                    g_vrcam_view_h.store(h, std::memory_order_release);
                }
                // Bind MAIN's live environment handle onto VRCAM using the engine's
                // OWN refcounted handle-assign (AddRef) -> exposure/tonemap/bloom
                // exactly like main, dynamic, and crash-safe (no raw-ptr aliasing).
                if (g_main_ctx && g_handle_assign) {
                    bool bound = false;
                    for (uint32_t off : kEnvHandleOffs) {
                        void** src = reinterpret_cast<void**>(g_main_ctx + off);
                        if (src[0]) {   // only when MAIN currently holds an env handle
                            g_handle_assign(reinterpret_cast<void*>(a3 + off),
                                            reinterpret_cast<void*>(g_main_ctx + off));
                            bound = true;
                        }
                    }
                    if (bound) ++CyberpunkVR_DebugRttEnvBindHits;
                    // The extra candidates, same mechanism, opt-in per slot.
                    const uint32_t xm = CyberpunkVR_EnvExtraMask;
                    for (uint32_t k = 0; k < kEnvExtraCount && xm; ++k) {
                        if (!(xm & (1u << k))) continue;
                        const uint32_t off = kEnvExtraOffs[k];
                        void** src = reinterpret_cast<void**>(g_main_ctx + off);
                        if (!src[0]) continue;
                        g_handle_assign(reinterpret_cast<void*>(a3 + off),
                                        reinterpret_cast<void*>(g_main_ctx + off));
                        ++CyberpunkVR_DebugEnvExtraBinds;
                    }
                }
                // (VRCAM camera fov/zoom force = MAIN is done in Detour_SlConstants, the
                // per-frame camera writer -> smooth, same ctx.)
                // IPD stereo for vrcam (RIGHT eye) is applied in Detour_SlConstants too.
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { vrcam = false; }
    }
    __int64 res = g_orig_flag_compute(a1, a2, a3, a4);
    if (res && vrcam && g_force_view_flags.load(std::memory_order_relaxed)) {
        __try {
            // (Optional / default-OFF) Force VRCAM's feature flags to the current
            // main view's. This makes VRCAM run the FULL main pass set, but that
            // includes VIEW-DEPENDENT global passes (shadow cascade regen, GI)
            // that write SHARED resources main also reads -> main shadow flicker.
            // Left off: VRCAM uses its own natural flags; rect + env-bind already
            // give it lighting + exposure/tonemap without touching main's shadows.
            uint64_t m0 = 0, m1 = 0;
            if (g_main_ctx) {
                m0 = *reinterpret_cast<uint64_t*>(g_main_ctx + 0x17D0);
                m1 = *reinterpret_cast<uint64_t*>(g_main_ctx + 0x17D8);
            }
            if ((m0 | m1) == 0) { m0 = CyberpunkVR_DebugFgMainF0; m1 = CyberpunkVR_DebugFgMainF1; }
            if (m0 | m1) {
                uint64_t* f = reinterpret_cast<uint64_t*>(res);
                // FIX: flags = main, but reuse main's view-dependent global shadow/
                // lighting structures instead of letting VRCAM rebuild the shared
                // buffers (which shifts/flickers them for main):
                // RIGOROUS reuse mechanism (verified via RE of each work-fn):
                //   bit 50 SET   -> cascade reuse (builder sub_141D43040 references the
                //                   EXISTING cascade atlas instead of building; TRUE reuse).
                //   bit 31 CLEAR -> GI reuse (work sub_14077E664 gates ONLY the update
                //                   sub_14077F758; the apply sub_14077E74C + global GI data
                //                   at renderer+184 run/persist regardless -> reuse main's GI).
                //   bit 11 KEPT SET -> distant shadows ENABLED for vrcam (its lighting
                //                   samples the distant map). bit 11 gates the WHOLE distant
                //                   node sub_140373998 (render + shared distant-manager state
                //                   advance) with no reuse-only sub-gate, so we instead keep
                //                   the bit set and SKIP vrcam's distant node via a dedicated
                //                   hook (Detour_DistantWork) -> vrcam neither advances the
                //                   shared manager (no ~1Hz shift) nor rebuilds -> it reuses
                //                   main's distant result. (GI-style reuse done through a hook
                //                   because distant lacks GI's update-only sub-gate.)
                uint64_t vf0;
                switch (CyberpunkVR_VrcamFlagMode) {
                    case 1:  vf0 = m0; break;                                        // exact main (own cascades+GI)
                    case 2:  vf0 = (m0 | SHADOW_CASCADE_REUSE_BIT); break;           // cascade reuse only, GI native
                    case 3:  vf0 = m0 & ~GI_FEATURE_BIT; break;                      // GI reuse only, cascade native
                    default: vf0 = (m0 | SHADOW_CASCADE_REUSE_BIT) & ~GI_FEATURE_BIT; // both reuse (current)
                }
                if (CyberpunkVR_DistantReuseMode == 0)
                    vf0 &= ~DISTANT_SHADOW_BIT;   // A/B: distant OFF for vrcam
                f[0] = vf0;
                f[1] = m1;
                CyberpunkVR_DebugFgRttF0 = f[0];
                CyberpunkVR_DebugFgRttF1 = f[1];
                ++CyberpunkVR_DebugRttFlagForceHits;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    // VRCAM final-color extraction fix: force build-bit 64 (f1 bit 0) so the scene
    // builder emits ExtractionSceneColor + ExtractionFinalColor for VRCAM, writing the
    // final-color that VRCAM's (unconditional) CopyToTexture then copies. Applied LAST
    // so it survives the flag-force block above. Gated + default OFF.
    if (res && vrcam && CyberpunkVR_VrcamExtractionFix) {
        __try {
            uint64_t* f = reinterpret_cast<uint64_t*>(res);
            f[1] |= 1ULL;                       // bit 64 -> sub_1407305B0(a4,64) == true
            CyberpunkVR_DebugFgRttF1 = f[1];
            ++CyberpunkVR_DebugVrcamExtractionHits;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    // UPSCALER SELECTOR (primary): capture MAIN's chosen upscaler (key==0), and for
    // vrcam force group 69 (DLSS) + clear 71/72/73 so SCENE_FULL's builder does NOT emit
    // the native TAAU/resolve (PipelineState_563) that crops. Runs LAST (survives the
    // main-flag-force above), independent of g_rtt_force_flags (re-reads the key), gated
    // on VrcamDlss ONLY, and only when MAIN itself selected DLSS. See FG_UPSCALER block.
    if (res) {
        __try {
            uint64_t key2 = a3 ? *reinterpret_cast<uint64_t*>(a3 + 0x28) : ~0ULL;
            uint64_t* f = reinterpret_cast<uint64_t*>(res);
            if (key2 == 0) {
                g_main_upscaler_groups.store(f[1] & FG_UPSCALER_MASK_F1, std::memory_order_release);
                CyberpunkVR_DebugMainUpscalerGroups = f[1] & FG_UPSCALER_MASK_F1;
                // AND THIS IS WHERE VRCAM'S DLSS DECIDES ITSELF. The whole feature is "mirror
                // whatever MAIN's upscaler is", and every gate below already refused to act unless
                // MAIN had group 69 -- so a separate switch could only ever be the wrong half of
                // an AND. Reading it off MAIN's own build flags removes the second thing to get
                // right, and it tracks the graphics menu live: turn DLSS off in the game and the
                // next graph build clears this, which unsticks vrcam's eval flag in Detour_ApplyDlss.
                const int32_t want = (f[1] & FG_DLSS_BIT_F1) ? 1 : 0;
                if (want != CyberpunkVR_VrcamDlss) {
                    CyberpunkVR_VrcamDlss = want;
                    log("[dlss] MAIN upscaler groups=%llX -> VRCAM DLSS %s (automatic)",
                        (unsigned long long)(f[1] & FG_UPSCALER_MASK_F1), want ? "ON" : "off");
                }
            } else if (key2 == g_vrcam_ctx_key && CyberpunkVR_VrcamDlss &&
                       (g_main_upscaler_groups.load(std::memory_order_acquire) & FG_DLSS_BIT_F1)) {
                uint64_t nf1 = (f[1] & ~FG_UPSCALER_MASK_F1) | FG_DLSS_BIT_F1;
                if (nf1 != f[1]) {
                    f[1] = nf1;
                    CyberpunkVR_DebugFgRttF1 = nf1;
                    ++CyberpunkVR_DebugUpscalerForceHits;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return res;
}

// The viewport-rect computer. Its caller does: call sub_1404E3EB4 ; movups
// xmm0,[rax] ; movdqu [ctx+0x14],xmm0. For the VRCAM view (flagged by the
// FlagCompute hook that ran immediately before) the input viewport is empty so
// the result is (0,0,0,0), which makes the engine skip the entire lighting
// composite. Overwrite the result with the full RTT rect (0,0,W,H) so the rect
// written into ctx+0x14 is valid -> full lighting flags + resources + integrate.
constexpr uintptr_t RECT_COMPUTE_RVA = 0x4E3EB4;   // sub_1404E3EB4
using RectComputeFn = __int64(__fastcall*)(void*, void*, void*);
static RectComputeFn g_orig_rect_compute = nullptr;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugRttRectHits = 0;

static __int64 __fastcall Detour_RectCompute(void* a1, void* a2, void* a3) {
    __int64 res = g_orig_rect_compute(a1, a2, a3);
    if (res && t_vrcam_setup) {
        t_vrcam_setup = false;   // consume: only the VRCAM view's rect
        __try {
            uint32_t* r = reinterpret_cast<uint32_t*>(res);
            r[0] = 0;            // left
            r[1] = 0;            // top
            r[2] = t_vrcam_w;    // right
            r[3] = t_vrcam_h;    // bottom
            ++CyberpunkVR_DebugRttRectHits;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return res;
}

// --- Distant-shadow reuse for VRCAM --------------------------------------
// CRenderNode_RenderDistantShadows work sub_140373998 gates its ENTIRE body on
// feature bit 11 (0x800 @ctx+0x17D0). Unlike GI (bit 31 gates only the update
// sub_14077F758, apply runs regardless = clean reuse) distant has NO reuse-only
// sub-gate: clearing bit 11 removes vrcam's distant shadows; setting it makes
// vrcam ADVANCE the SHARED distant manager (*(node_data+160), incremental ~1Hz
// slice state machine via sub_140374B48) -> desyncs main's incremental update ->
// ~1Hz sun/shadow shift. Reuse = keep bit 11 SET (vrcam's lighting still samples
// the distant map) but SKIP vrcam's distant node entirely so it neither advances
// the shared manager nor re-renders -> vrcam reuses main's distant result while
// main stays clean. Work-fn ABI: a2=rdx, view ctx = *(a2+0x18), key @ctx+0x28.
// Both distant nodes advance the SHARED distant manager (*(node_data+160)) via
// sub_140374B48: RenderDistantShadows sub_140373998 AND PrepareDistantShadows
// sub_140374AD8. Skip BOTH for vrcam so it never touches the shared state.
// ---- the amortised sky, and the second view fighting MAIN for it ---------------------------
//
// PROVEN, not inferred. sub_1407818F8 (the body of CRenderNode_RenderSkyScattering) builds the
// sky in SIX INSTALMENTS into a 32-byte record picked by `32 * *(BYTE*)(view + 0x16E0)`:
//
//     v15 = *(BYTE*)(rec + 80);                  // the shared slot cursor
//     do { v16 = 1 << v15++; v17 = v14 & v16; } while (!v17);
//     *(BYTE*)(rec + 80) = v15;                  // ADVANCE IT
//     sub_140783384(a2, v17, ...);               // fill that slot FROM THIS VIEW
//     if (*(BYTE*)(rec + 80) >= 6) { publish; slot = 0; InterlockedExchange(rec+72, 0); }
//
// and the live probe says both views index the SAME record:
//     [sky] sky-record index view+0x16E0 -- MAIN 0  VRCAM 0     AA mode -- MAIN 0  VRCAM 0
//
// AA mode 0 on both means `!v4` holds for both, so both views pass the gate and both advance one
// cursor. The published sky is therefore assembled from alternating instalments of two different
// cameras -- MAIN fills slot 0, VRCAM slot 1, MAIN slot 2, and so on -- and it reaches six in
// half the frames, so it republishes twice as often, each time half-wrong. Which view lands on
// which slot drifts, so the result changes shape from frame to frame. At night that LUT is what
// carries the stars and the horizon, which is exactly the reported "no stars, the sky is not
// like that". The enabled-slot mask itself is per view too (`v14 = v28 ? -1 : -17`, and v28 is
// sub_140B2CB98(a2, ...)), so the two are not even filling the same set.
//
// The remedy is the one distant shadows and local shadow maps both needed, for the same reason:
// let ONE view drive the shared structure and have the other consume the published result. The
// sky LUT is a function of sun direction and altitude, not of view direction, so MAIN's answer
// is correct for an eye 6.5 cm away -- the same argument that makes cascade and GI reuse sound.
//
// 1 = skip the sky build for the second view (it samples MAIN's published sky).
// 0 = both views build it, i.e. the shipped behaviour, for A/B.
constexpr uintptr_t SKY_WORK_RVA = 0x7818F8;   // sub_1407818F8, the body behind feature 35
using SkyWorkFn = void(__fastcall*)(void*, void*);
static SkyWorkFn g_orig_sky_work = nullptr;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_SkyReuseMode = 0;   // A/B: does the second view get its cloud state back?
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugSkySkipHits = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugSkyMainHits = 0;

static void __fastcall Detour_SkyWork(void* a1, void* a2) {
    bool vrcam = false;
    if (a2) {
        __try {
            const uintptr_t ctx = *reinterpret_cast<uintptr_t*>(
                reinterpret_cast<uint8_t*>(a2) + 0x18);
            vrcam = ctx && *reinterpret_cast<uint64_t*>(ctx + 0x28) == g_vrcam_ctx_key;
        } __except (EXCEPTION_EXECUTE_HANDLER) { vrcam = false; }
    }
    if (vrcam) {
        if (CyberpunkVR_SkyReuseMode == 1) {
            InterlockedIncrement64(
                reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugSkySkipHits));
            return;
        }
    } else {
        InterlockedIncrement64(
            reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugSkyMainHits));
    }
    g_orig_sky_work(a1, a2);
}

constexpr uintptr_t DISTANT_RENDER_RVA  = 0x373998;   // sub_140373998
constexpr uintptr_t DISTANT_PREPARE_RVA = 0x374AD8;   // sub_140374AD8
using DistantWorkFn = void(__fastcall*)(void*, void*, void*);
static DistantWorkFn g_orig_distant_render  = nullptr;
static DistantWorkFn g_orig_distant_prepare = nullptr;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugDistantSkipHits = 0;

// SEH-only helper (no C++ objects)  view ctx = *(a2+0x18), key @ctx+0x28.
// Only skip when distant-reuse mode is active (1); mode 0 leaves distant to the
// engine's own bit-11-clear no-op so the A/B baseline is the vanilla path.
static bool distant_is_vrcam(void* a2) {
    if (CyberpunkVR_DistantReuseMode != 1 || !a2) return false;
    __try {
        uintptr_t ctx = *reinterpret_cast<uintptr_t*>(
            reinterpret_cast<uint8_t*>(a2) + 0x18);
        return ctx && *reinterpret_cast<uint64_t*>(ctx + 0x28) == g_vrcam_ctx_key;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void __fastcall Detour_DistantRender(void* a1, void* a2, void* a3) {
    if (distant_is_vrcam(a2)) { ++CyberpunkVR_DebugDistantSkipHits; return; }
    g_orig_distant_render(a1, a2, a3);
}
static void __fastcall Detour_DistantPrepare(void* a1, void* a2, void* a3) {
    if (distant_is_vrcam(a2)) { ++CyberpunkVR_DebugDistantSkipHits; return; }
    g_orig_distant_prepare(a1, a2, a3);
}

// --- Volumetric clouds: hand VRCAM MAIN's cloud constants ----------------------------
// MEASURED live in both views (x64dbg, breakpoint inside CRenderNode_RenderVolumetricClouds
// sub_14061B5B4 right after it resolves its view object): the cloud parameter block at
// viewData+0x550 is byte-identical between MAIN and VRCAM for its first 0x9C bytes, and then
// differs in EXACTLY six floats at +0x9C..+0xB3 -- the wind-scroll offsets of the three
// cloud-noise octaves. MAIN carried the offsets accumulated over the session,
//     (-29.50, -255.00)   (-39.29, -412.05)   (-49.58, -433.98)
// while VRCAM's were ZERO and stayed zero: the engine only ever advances the primary view's.
// sub_140784654 multiplies those six by 0.1 into the cloud constant buffer as the UV offsets
// of the three noise layers, so the two eyes sample the cloud field in completely different
// places -- different shapes, a cloud present in one eye and absent in the other, and no
// convergence while standing still (which is what ruled temporal accumulation out).
//
// It has to be done per frame: the view object comes from a pool and is a different address
// every frame (proven -- a one-shot poke of the six floats was gone by the next hit), and the
// fresh one always arrives zeroed.
//
// ONLY those six values are mirrored. Copying the whole buffer was tried first and is WRONG:
// it makes VRCAM's clouds disappear entirely (tested live, both variants). The buffer also
// carries fields that are genuinely per-view -- +140/+160/+164 come out of the frame's
// resource resolve (sub_1401F3D20), i.e. descriptor indices for TRANSIENT targets, so MAIN's
// point at memory that has been aliased to something else by the time VRCAM's pass runs.
// +168/+172 are that view's jitter and +144/+148 its resolved cloud-target size.
//
// Blast radius is clouds and nothing else: sub_140784654's only caller in the 169-node graph
// is the volumetric-clouds node, and viewData+0x430/+0x550 are read by no other node either.
// Size is exact, not assumed -- the caller follows the fill with sub_1401EE3CC(0xC0, Src).
constexpr uintptr_t CLOUD_CB_FILL_RVA = 0x784654;   // sub_140784654
constexpr size_t    CLOUD_CB_BYTES    = 192;        // 0xC0, per the upload right after it
using CloudCbFn = __int64(__fastcall*)(__int64, __int64, __int64, __int64, __int64,
                                       int*, int, int);
static CloudCbFn g_orig_cloud_cb = nullptr;
// 3 = mirror ONLY the three noise-layer offsets -- the measured difference  [default, proven]
// 0 = off (engine's own per-view constants, i.e. the broken-stereo baseline)
// 1 = mirror MAIN's buffer, keep VRCAM's own view size + jitter   -- kills VRCAM's clouds
// 2 = mirror MAIN's buffer verbatim                               -- kills VRCAM's clouds
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_CloudCbMode = 3;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCloudCbMain  = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCloudCbVrcam = 0;
static std::mutex g_cloud_cb_mtx;
static uint8_t    g_cloud_cb_main[CLOUD_CB_BYTES];
static std::atomic<bool> g_cloud_cb_have{false};

// SEH-only helpers below: they touch engine-owned memory on job threads, and __try cannot
// share a function with C++ object unwinding (C2712), so the locking stays out of them.
static bool cloud_cb_raw_copy(void* dst, const void* src, size_t n) {
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// AS MUCH AS IS ACTUALLY THERE. The viewData capture was all-or-nothing on 0xFF0 bytes and it
// never landed -- no [viewData] line has ever been logged, while the graph-context diff beside
// it reports fine. 0xFF0 came from the HUD reversing and evidently overruns the object on this
// path. Copy in 64-byte steps and return how far we got, so a short object still gets diffed.
static size_t raw_copy_upto(void* dst, const void* src, size_t n) {
    size_t done = 0;
    while (done < n) {
        const size_t step = (n - done) < 64 ? (n - done) : 64;
        if (!cloud_cb_raw_copy(static_cast<uint8_t*>(dst) + done,
                               static_cast<const uint8_t*>(src) + done, step)) break;
        done += step;
    }
    return done;
}

// a3 is the node work-context: view ctx = *(a3+0x18), key @ ctx+0x28. MAIN is 0, VRCAM is
// g_vrcam_ctx_key; every other view (shadow, reflection, ...) is left entirely alone.
static int cloud_cb_view(__int64 a3) {
    if (!a3) return -1;
    __try {
        uintptr_t ctx = *reinterpret_cast<uintptr_t*>(
            reinterpret_cast<uint8_t*>(a3) + 0x18);
        if (!ctx) return -1;
        const uint64_t key = *reinterpret_cast<uint64_t*>(ctx + 0x28);
        if (key == 0) return 0;
        if (key == g_vrcam_ctx_key) return 1;
        return -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static void cloud_cb_capture_main(__int64 cb) {
    uint8_t tmp[CLOUD_CB_BYTES];
    if (!cb || !cloud_cb_raw_copy(tmp, reinterpret_cast<const void*>(cb), CLOUD_CB_BYTES))
        return;
    {
        std::lock_guard<std::mutex> lk(g_cloud_cb_mtx);
        memcpy(g_cloud_cb_main, tmp, CLOUD_CB_BYTES);
    }
    g_cloud_cb_have.store(true, std::memory_order_release);
    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugCloudCbMain));
}

// ONLY 0x40. The buffer's other differing fields are per-view by construction and the mode-1
// path below already says so: +152 is this view's width/height and +168 its jitter, both
// deliberately preserved there. 0xA8 IS +168, so mirroring it -- which the first version of
// this did -- hands VRCAM MAIN's jitter. 0x90 (+144) is the resolved cloud-target size, which
// is why it reads {M 1 | V 512}. +0x8C/+0xA0/+0xA4 are transient descriptor indices and
// mirroring the buffer wholesale is already known to kill VRCAM's clouds outright.
//   bit 0  0x40..0x4F -- the WHOLE float4, not just the first dword. The diff prints where
//          a run starts, never how long it is, and 4 bytes was the same off-by-one that cost
//          five rounds on viewData. The raymarch reads _40_m0[4].w -- byte 0x4C -- as the
//          light-intensity multiplier: `_412 = _40_m0[4].w * _30_m0[6].x` for each channel,
//          i.e. a flat scale on the cloud colour. That is the shape of "always lighter".
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_CloudCbExtra = 1;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCloudCbExtra = 0;

// Report-only diff of the whole 192 bytes, taken BEFORE anything is written -- which is why 0x50
// reads as a hole here: the wind mirror fills it a few lines further down.
static void block_diff_log(const char* tag, const uint8_t* refb, const uint8_t* curb,
                           size_t bytes);
static void cloud_cb_diff_vrcam(__int64 cb) {
    static uint64_t s_last = 0;
    if (!cb || !g_cloud_cb_have.load(std::memory_order_acquire)) return;
    const uint64_t now = GetTickCount64();
    if (s_last && now - s_last < 15000) return;
    uint8_t cur[CLOUD_CB_BYTES], ref[CLOUD_CB_BYTES];
    if (!cloud_cb_raw_copy(cur, reinterpret_cast<const void*>(cb), CLOUD_CB_BYTES)) return;
    {
        std::lock_guard<std::mutex> lk(g_cloud_cb_mtx);
        memcpy(ref, g_cloud_cb_main, CLOUD_CB_BYTES);
    }
    s_last = now;
    block_diff_log("cloudCB", ref, cur, CLOUD_CB_BYTES);
}


static void cloud_cb_apply_vrcam(__int64 cb) {
    // No MAIN snapshot yet (first frames, or MAIN's clouds gated off) -> leave the engine's
    // own constants alone. An optional input must degrade, never blank the pass.
    if (!cb || !g_cloud_cb_have.load(std::memory_order_acquire)) return;
    const uint32_t mode = CyberpunkVR_CloudCbMode;
    uint8_t tmp[CLOUD_CB_BYTES];
    {
        std::lock_guard<std::mutex> lk(g_cloud_cb_mtx);
        memcpy(tmp, g_cloud_cb_main, CLOUD_CB_BYTES);
    }
    if (mode == 3) {
        if (CyberpunkVR_CloudCbExtra & 1)
            cloud_cb_raw_copy(reinterpret_cast<uint8_t*>(cb) + 0x40, tmp + 0x40, 16);
        if (CyberpunkVR_CloudCbExtra)
            InterlockedIncrement64(
                reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugCloudCbExtra));
        // The three noise-layer offsets land at CB+80..+103 (a5+156..+176, each x0.1).
        if (cloud_cb_raw_copy(reinterpret_cast<uint8_t*>(cb) + 80, tmp + 80, 24))
            InterlockedIncrement64(
                reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugCloudCbVrcam));
        return;
    }
    if (mode == 1) {
        uint8_t keep[16];
        if (cloud_cb_raw_copy(keep, reinterpret_cast<const uint8_t*>(cb) + 152, 8) &&
            cloud_cb_raw_copy(keep + 8, reinterpret_cast<const uint8_t*>(cb) + 168, 8)) {
            memcpy(tmp + 152, keep, 8);       // this view's width/height
            memcpy(tmp + 168, keep + 8, 8);   // this view's jitter, in its own NDC
        }
    }
    if (cloud_cb_raw_copy(reinterpret_cast<void*>(cb), tmp, CLOUD_CB_BYTES))
        InterlockedIncrement64(
            reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugCloudCbVrcam));
}

// --- what ELSE of the view block does the RTT view not get? --------------------------------
// The cloud wind offsets turned out to be one instance of a pattern: sub-blocks of viewData
// that the engine only ever fills for the primary view. ScreenSpaceRain is measurably another
// -- it runs for VRCAM but early-outs, because its gate is
//     sub_1401ED930(wc) + 0xAB0, floats at +52 / +72 / +76, at least one > 0
// and those are zero for VRCAM (audit: the node is 119x cheaper there). So rather than chase
// them one symptom at a time, diff the WHOLE view block once and let it name every hole.
// Runs off the cloud hook because that already has viewData in hand for both views
// (a4 == viewData + 0x430), so no extra engine call and no new hook.
// MODE 2 for one run. Mode 1 lists only MAIN-set/VRCAM-zero holes, and the night difference is
// not a hole -- both views have values, they are simply not the same values. The user reports
// that VRCAM does not follow the dusk-to-dawn transition at all, which is a TIME failure: the
// weather/time-of-day blend reaches MAIN's view block and not the second one. Mode 2 prints
// every differing run, so the extent of that block gets named instead of guessed at.
extern "C" __declspec(dllexport) int32_t CyberpunkVR_ViewDataDiff = 2;   // OFF: 33 MB and 16693 lines per session
constexpr size_t VIEWDATA_BYTES = 0xFF0;      // the view object's size, from the HUD reversing
static uint8_t g_vd_main[VIEWDATA_BYTES];
static std::atomic<bool> g_vd_have{false};

// The MAIN-set / VRCAM-zero ranges the diff reported live, with what the static reverse says
// reads them (traced by following sub_1401ED930's result through every decompiled node body):
//   0x150, 0x350        -> ScreenSpaceRain's block (base 0x334 / 0x4C)
//   0x168, 0x1A0, 0x1DC, 0x268 -> ONE block based at 0x168, read by CompositionPostProcess,
//        DrawComposition and GenerateTonemappingLUT. This is the same +0x168 those three nodes
//        gate on and that crashed twice when faked -- it is an output-resource set, not data.
//   0x5EC               -> the cloud wind offsets (already fixed via the cloud CB)
//   0xAF8               -> ScreenSpaceRain's wetness gate -- PROVEN: filling it brings the
//                          puddles back
//   0xF88, 0xFCC, 0xFE0 -> one block based at 0xF80, read by CompositionPostProcess and
//        RenderDebugSystems
struct ViewDataHole { uint16_t off, len; const char* note; };
static const ViewDataHole kViewDataHoles[] = {
    { 0x150,  4, "rain block" },      // bit 0
    { 0x168, 16, "composition out" }, // bit 1  -- DANGEROUS: resource set, see above
    { 0x1A0,  4, "composition out" }, // bit 2
    { 0x1DC,  8, "composition out" }, // bit 3
    { 0x268,  4, "composition out" }, // bit 4
    { 0x350, 12, "rain block" },      // bit 5
    { 0x5EC, 24, "cloud wind" },      // bit 6  -- already handled via the cloud CB
    { 0xAF8,  8, "rain wetness" },    // bit 7  -- PROVEN
    { 0xF88,  8, "composition/debug" },// bit 8
    { 0xFCC,  4, "composition/debug" },// bit 9
    { 0xFE0, 12, "composition/debug" },// bit 10
    // FOUND AT NIGHT, 2026-07-31. The table above was built in daylight and these two never
    // appeared in it. Both are plain floats -- no pointer anywhere near them -- so they are in
    // the safe class, unlike the 0x168 resource set.
    //   0x1CC  {2.4278, 118241.1, 118241.1}
    //   0x4A0  {400000.0}   <- a lone large float, and VRCAM has ZERO there
    // 400000 with nothing on the other side is the shape of a far/streaming distance, and a far
    // distance of zero is precisely "the second eye shows nothing in the distance". Read as a
    // squared distance it is 632 m; as centimetres, 4 km. Either is a plausible city far plane.
    { 0x1CC, 12, "night pair" },      // bit 11
    { 0x4A0,  4, "no reader" },       // bit 12  -- 400000.0, but nothing reads it
};

// Shared by the viewData and graph-context diffs: report the dword runs that differ, starring
// the ones where MAIN has a value and the RTT view has nothing -- those are the sub-blocks the
// engine never fills for it, which is the whole class of bug this chases.
static void block_diff_log(const char* tag, const uint8_t* refb, const uint8_t* curb,
                           size_t bytes) {
    const uint32_t* m = reinterpret_cast<const uint32_t*>(refb);
    const uint32_t* v = reinterpret_cast<const uint32_t*>(curb);
    const size_t nd = bytes / 4;
    char line[4000];
    int used = 0, runs = 0, holes = 0;
    line[0] = '\0';
    for (size_t i = 0; i < nd; ) {
        if (m[i] == v[i]) { ++i; continue; }
        const size_t s = i;
        while (i < nd && m[i] != v[i]) ++i;
        ++runs;
        bool vr_zero = true, main_set = false;
        for (size_t k = s; k < i; ++k) { if (v[k]) vr_zero = false; if (m[k]) main_set = true; }
        const bool hole = vr_zero && main_set;
        if (hole) ++holes;
        // Printing only the holes was a real blind spot: it hides every field where the second
        // view has its OWN non-zero value, which is precisely the shape of the grading-LUT
        // selection (MAIN picks Resource_345, VRCAM picks Resource_2123) and of any other
        // per-view setting that is chosen rather than left empty. Mode 2 prints them all.
        const bool want = (CyberpunkVR_ViewDataDiff >= 2) ? true : hole;
        // BOTH SIDES, and never truncated. Mode 2 used to print only MAIN's dwords into one
        // 1700-char line, which answered "these ranges differ" but not the question that matters
        // for a stale-environment theory: does the SECOND view's value move at all between two
        // samples? Print m|v for the first dword of every run and flush whenever the line fills,
        // so a night capture and a dawn capture can be held side by side.
        if (want) {
            if (used > static_cast<int>(sizeof(line)) - 48) {
                log("[vdiff] %s M|V cont: %s", tag, line);
                used = 0; line[0] = 0;
            }
            used += snprintf(line + used, sizeof(line) - used, "%X{%08X|%08X}%s ",
                             static_cast<unsigned>(s * 4), m[s], v[s], hole ? "H" : "");
        }
    }
    if (CyberpunkVR_ViewDataDiff >= 2) {
        log("[vdiff] %s M|V %d runs (%d holes, H marked) END: %s",
            tag, runs, holes, used ? line : "(none)");
    } else {
        log("[vdiff] %s MAIN vs VRCAM: %d differing runs, %d MAIN-set/VRCAM-zero: %s",
            tag, runs, holes, holes ? line : "(none)");
    }
}

// The graph CONTEXT is the other place per-view state lives, and it is where the remaining
// symptom has to be: the viewData holes are all accounted for above and none of them is
// lighting. Same treatment -- capture MAIN's, diff VRCAM's, report the holes.
constexpr size_t CTX_BYTES = 0x2000;      // covers everything we have ever seen used (0x1E10+)
static uint8_t g_ctx_main[CTX_BYTES];
static std::atomic<bool> g_ctx_have{false};

static void ctx_capture_main(__int64 ctx) {
    uint8_t tmp[CTX_BYTES];
    if (!ctx || !cloud_cb_raw_copy(tmp, reinterpret_cast<const void*>(ctx), CTX_BYTES)) return;
    {
        std::lock_guard<std::mutex> lk(g_cloud_cb_mtx);
        memcpy(g_ctx_main, tmp, CTX_BYTES);
    }
    g_ctx_have.store(true, std::memory_order_release);
}

static void ctx_diff_vrcam(__int64 ctx) {
    if (!ctx || !g_ctx_have.load(std::memory_order_acquire)) return;
    uint8_t cur[CTX_BYTES], ref[CTX_BYTES];
    if (!cloud_cb_raw_copy(cur, reinterpret_cast<const void*>(ctx), CTX_BYTES)) return;
    {
        std::lock_guard<std::mutex> lk(g_cloud_cb_mtx);
        memcpy(ref, g_ctx_main, CTX_BYTES);
    }
    block_diff_log("graphCtx", ref, cur, CTX_BYTES);
    // ALL the frame-graph feature words, not just f0/f1. sub_14023AF5C indexes them as
    //     *(ctx + 8*(bit >> 6) + 6096)
    // so the bitset runs well past the two words this project has always logged and forced. A
    // bit MAIN has and VRCAM lacks in word 2 or beyond would gate a node exactly like bit 25
    // gates the clouds -- and the whole-context diff above cannot show it, because that diff
    // only flags ranges where VRCAM is ZERO, and a word that is merely missing one bit is not.
    char fl[1200];
    int fu = 0;
    fl[0] = 0;
    for (int w = 0; w < 24 && fu < static_cast<int>(sizeof(fl)) - 64; ++w) {
        uint64_t m = 0, v = 0;
        memcpy(&m, ref + 6096 + w * 8, 8);
        memcpy(&v, cur + 6096 + w * 8, 8);
        if (m == v) continue;
        fu += snprintf(fl + fu, sizeof(fl) - fu, "f%d m=%016llX v=%016llX miss=%016llX ", w,
                       (unsigned long long)m, (unsigned long long)v,
                       (unsigned long long)(m & ~v));
    }
    log("[fgflags-all] words differing between the views (miss = MAIN has, VRCAM lacks): %s",
        fu ? fl : "(none)");
}

static std::atomic<size_t> g_vd_len{0};
static void viewdata_capture_main(__int64 vd) {
    uint8_t tmp[VIEWDATA_BYTES];
    if (!vd) return;
    const size_t got = raw_copy_upto(tmp, reinterpret_cast<const void*>(vd), VIEWDATA_BYTES);
    if (got < 256) return;
    {
        std::lock_guard<std::mutex> lk(g_cloud_cb_mtx);
        memcpy(g_vd_main, tmp, got);
    }
    g_vd_len.store(got, std::memory_order_release);
    g_vd_have.store(true, std::memory_order_release);
}

// Logged on a timer, not once, so the scene can be changed (step into the rain, walk up to the
// stalls) and a fresh answer read out without a restart.
static void viewdata_diff_vrcam(__int64 vd) {
    static uint64_t s_last = 0;
    if (!vd || !g_vd_have.load(std::memory_order_acquire)) return;
    const uint64_t now = GetTickCount64();
    if (s_last && now - s_last < 15000) return;
    uint8_t cur[VIEWDATA_BYTES], ref[VIEWDATA_BYTES];
    size_t n = raw_copy_upto(cur, reinterpret_cast<const void*>(vd), VIEWDATA_BYTES);
    const size_t mainLen = g_vd_len.load(std::memory_order_acquire);
    if (n > mainLen) n = mainLen;
    n &= ~size_t(3);
    if (n < 256) { log("[viewData] unreadable: got %zu bytes of %u", n, (unsigned)VIEWDATA_BYTES);
                   s_last = now; return; }
    {
        std::lock_guard<std::mutex> lk(g_cloud_cb_mtx);
        memcpy(ref, g_vd_main, n);
    }
    s_last = now;
    block_diff_log("viewData", ref, cur, n);
    // THE FOG-OVERLAY INPUTS, BY NAME. sub_14061F9E0 (RenderFogOverlay) reads a float block at
    // viewData+0x8C0 and gates the whole distant-fog path on viewData+0x920 > 0.0:
    //     if ((f0 & 0x800000) == 0 && !sub_1401E4B60(ctx+20) || *(float*)(vd+2336) <= 0.0) v6 = 0;
    //     if ((f0 & 0x1000000) || (f0 & 0x4000000) || v6) { ...fog... }
    //     v19 = (float*)(vd + 2240);  v21 = *(vd+2332) * *(float*)(vd+2328);
    // These are not holes -- both views have values there -- so the hole report above cannot see
    // them. A fog that is too strong hides the far city AND washes the stars out of the sky,
    // which is one cause for both reported symptoms, so print the two sides and compare.
    if (n >= 0x928) {
        char fl[700];
        int u = 0;
        fl[0] = 0;
        static const uint16_t kFogOff[] = { 0x8C0, 0x8C4, 0x8C8, 0x8CC, 0x8D0, 0x8D4,
                                            0x918, 0x91C, 0x920, 0x924 };
        for (size_t k = 0; k < sizeof(kFogOff) / sizeof(kFogOff[0]); ++k) {
            float fm = 0.0f, fv = 0.0f;
            memcpy(&fm, ref + kFogOff[k], 4);
            memcpy(&fv, cur + kFogOff[k], 4);
            if (u < static_cast<int>(sizeof(fl)) - 64)
                u += snprintf(fl + u, sizeof(fl) - u, "%X{M %.4g|V %.4g}%s ",
                              kFogOff[k], fm, fv, (fm == fv) ? "" : " <<");
        }
        log("[fog] RenderFogOverlay inputs, MAIN|VRCAM (<< marks a difference): %s", fl);
    }
    // What is actually IN MAIN's holes decides whether a hole is safe to fill: a float is data,
    // a user-space address is a resource handle VRCAM does not own (viewData+0x168 is exactly
    // that -- the composition output set that crashed twice when it was faked).
    char vals[900];
    int used = 0;
    vals[0] = '\0';
    for (size_t i = 0; i < sizeof(kViewDataHoles) / sizeof(kViewDataHoles[0]); ++i) {
        const ViewDataHole& h = kViewDataHoles[i];
        float f0 = 0, f1 = 0;
        uint64_t q = 0;
        memcpy(&f0, ref + h.off, 4);
        if (h.len >= 8) { memcpy(&f1, ref + h.off + 4, 4); memcpy(&q, ref + h.off, 8); }
        if (used < static_cast<int>(sizeof(vals)) - 80)
            used += snprintf(vals + used, sizeof(vals) - used,
                             "b%zu@%X{%.4g,%.4g%s} ", i, h.off, f0, f1,
                             (h.len >= 8 && q >= 0x10000000000ull && q < 0x7FFFFFFFFFFFull)
                                 ? ",PTR" : "");
    }
    log("[vdiff] MAIN's values in the holes: %s", vals);
}

// --- filling the holes ---------------------------------------------------------------------
// The ranges the diff above reports as MAIN-set / VRCAM-zero, measured live. Copying MAIN's
// bytes into them cannot destroy per-view data by construction: VRCAM has nothing there.
// One bit each in CyberpunkVR_ViewDataFixMask so every one can be A/B'd live -- these are
// engine internals we have only partially identified, and a field that looks inert may be a
// count whose array VRCAM does not own.
// Default: the wetness only. It is the one hole whose consumer is identified from the engine's
// own code, so it is the only one that can be turned on without guessing.
// bits 11 and 12 stay OFF. 0x4A0 held MAIN's 400000.0 against VRCAM's zero and looked exactly
// like a far distance -- but of the 68 callers of the viewData getter sub_1401ED930, NOT ONE
// reads +0x4A0. Nothing consumes the field, so filling it is a no-op, which is what the live
// test showed. Kept in the table as a named negative so it is not rediscovered.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_ViewDataFixMask = (1u << 0) | (1u << 5) | (1u << 7);
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugViewDataFixes = 0;

// A hole we have not identified may hold a pointer, and MAIN's pointer in VRCAM's slot is a
// crash waiting for a dereference. Refuse anything that looks like a user-space address.
static bool viewdata_looks_like_pointer(const uint8_t* p, size_t len) {
    if (len < 8) return false;
    for (size_t i = 0; i + 8 <= len; i += 8) {
        uint64_t q;
        memcpy(&q, p + i, 8);
        if (q >= 0x10000000000ull && q < 0x7FFFFFFFFFFFull) return true;
    }
    return false;
}

// Runs on every VRCAM node dispatch, so it stages only the enabled ranges (max 24 bytes each)
// instead of the whole 4 KB block -- cheap enough to be unconditional, which is what makes the
// "before any consumer" guarantee hold without knowing who the consumers are.
// ---- the distant-fog switch the two views disagree on --------------------------------------
//
// Measured, both views, same frame, same spot:
//     0x8C0 {M 1.5 | V 3}   0x8C8 {M 0.75 | V 0.5}   0x8CC {M 1 | V 4}
//     0x918 {M 0.5 | V 1}   0x920 {M 0    | V 0.00025}
//
// 0x920 is not a parameter, it is THE SWITCH. From sub_14061F9E0 (RenderFogOverlay):
//     if ((f0 & 0x800000) == 0 && !sub_1401E4B60(ctx+20)
//         || (v6 = 1, *(float*)(vd + 0x920) <= 0.0))   v6 = 0;
//     if ((f0 & 0x1000000) || (f0 & 0x4000000) || v6) { ... if (v6) <the distant-fog branch> }
// v6 can only survive as 1 when vd+0x920 is strictly positive. MAIN holds exactly 0 there and so
// never enters that branch; VRCAM holds 0.00025 and always does. The second eye runs a
// distant-fog path the first eye does not have, with the parameters beside it 2x to 4x MAIN's.
// A fog the other eye lacks, at four times the strength, buries the far city and washes the
// stars out of the sky at once -- both reports, one field.
//
// Deliberately NOT part of the hole table. Those are safe because VRCAM has nothing in them;
// here it has real values, so copying MAIN's overwrites live per-view data. Start with the
// Start with the switch alone if you must, but all three by default: every measured field
// differed in the same direction, so leaving the parameters at VRCAM's 2x-4x only moves the
// problem to whichever fog path it does take. The risk is real and stated -- unlike a hole
// these overwrite live per-view values, and if one is resolution-derived (0x8CC reads
// M 1 | V 4, which has that shape) the second eye will look wrong in a new way.
//   bit 0   0x920            the switch
//   bit 1   0x918, 0x91C     the scale pair
//   bit 2   0x8C0 .. 0x8D7   the parameter block
// Live: 7 = all, 1 = switch only, 0 = untouched.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_FogMirrorMask = 7;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugFogMirrors = 0;
// WIDENED after the first attempt left the night still wrong. Six floats was not the block:
// MAIN's own branch in sub_14061F9E0 does `sub_140201A58(v19 + 35, ...)` with
// v19 = (float*)(viewData + 0x8C0), so the parameter array is 36 floats -- 0x8C0 .. 0x94F --
// and 0x910/0x914 are read by the other branch beside 0x918/0x91C/0x920. Bit 2 now covers all
// of it, which makes bits 0 and 1 subsets kept only for bisecting.
struct FogRange { uint16_t off, len; };
static const FogRange kFogMirror[3] = { { 0x920, 4 }, { 0x910, 0x14 }, { 0x8C0, 0x90 } };

// ---- the atmosphere block, mirrored in bisectable pieces -----------------------------------
//
// Four captures, two at night and two through dawn, printing BOTH views. They split the night
// difference in two, and only one half is weather.
//
// Constant across all four -- configuration, not time:
//     3F0 {M 1.365|V 4.55}   480 {M 1|V 0.75}    4D4 {M 0.03|V 0.06}
//     570 {M 0.7  |V 0.4 }   5A4 {M 1|V 0.766}   5C0 {M 1.3 |V 0.3 }
//     610 {M 8    |V 4   }   61C {M 0.05|V 0.03} 628 {M 0.65|V 1   }   6FC {M 1.4|V 1}
// 8 against 4 and 0.03 against 0.06 have the shape of a march step count and its step size: the
// engine hands the RTT view a cheaper atmosphere. That is the tint, the over-visible mountain
// silhouettes, and why daylight looks the same -- at noon those coefficients barely register.
//
// Moving, but not together -- the weather blend:
//     430 sky radiance  M 8.17 -> 8.62 -> 8.89 -> 131.0
//                       V 2.09 -> 2.09 -> 64.6 -> 68.1
//     3C0 colour        M 0.157 -> 0.155 -> 0.153 -> 0.051
//                       V 0.095 -> 0.095 -> 0     -> 0
// The second view is not frozen; it takes the transition early and on different values, and 3C0
// collapses to zero by dawn. MAIN's smooth 8 -> 131 arc against VRCAM's 2 -> 65 step is exactly
// the reported "there is no gradual lightening".
//
// So mirror MAIN's atmosphere block. This is a BROAD overwrite of live per-view values and it is
// not a root fix -- the root is whatever hands the RTT view its own configuration, which is not
// found yet. Split into bits so a bad range can be bisected out without a rebuild, and with two
// exclusions that are not negotiable: 0x6E0 is a pointer (21B04B08 | 212ACC20) and 0x120..0x143
// is the camera position, which MUST differ between eyes.
//   bit 0  0x370..0x3FF   sun direction and colours
//   bit 1  0x400..0x45F   radiance
//   bit 2  0x460..0x4FF   the 0x480 / 0x4C0 / 0x4D4 group
//   bit 3  0x500..0x5CF   the 0x554 / 0x570 / 0x598 / 0x5A4 / 0x5C0 group
//   bit 4  0x610..0x633 and 0x6FC   the march-step group
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_EnvMirrorMask = 0x7FF;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugEnvMirrors = 0;
// EXTENDED once the mirror shortened the list from 68 runs to 43 and the printer stopped
// truncating. Two things showed up. Three fields sat just past a boundary -- 0x5D0, 0x634,
// 0x700 -- and a whole second block lives beyond 0x700 that had never been visible:
//     830 {M 125 |V 250}   870 {M 0.3 |V 0.06}   8B0 {M -1  |V 0.015}
//     998 {M 10  |V 16 }   A14 {M 0.5 |V 0.25}   AC4 {M 0.4 |V 0.224}
//     AEC {M 1   |V 0  }   B20 {M 0.05|V 0.15}   B30 {M 0   |V 1}
// Same signature as the first block: constant in time, VRCAM systematically cheaper or stronger.
// That is the residue the user still sees -- a lighter distant background at night and a faint
// white haze in the distance by day.
//   bit 5  0x830..0x8B3     bit 6  0x950..0x967 and 0x998     bit 7  0xA14 and 0xAC4..0xAEF
//   bit 8  0xB20..0xB33
// NOT mirrored, deliberately: 0xBA0/0xBC0/0xBE0/0xBFC/0xC40 differ only in the last digits and
// drift together frame to frame (405B9BA0 against 405B9BB9) -- that is per-eye camera-derived
// data and copying it would break the stereo. 0x6E0, 0xF58, 0xF88, 0xFB8 are pointers/resources.
static const FogRange kEnvMirror[14] = {
    { 0x370, 0x90 }, { 0x400, 0x60 }, { 0x460, 0xA0 }, { 0x500, 0xE0 },
    { 0x610, 0x28 }, { 0x6FC, 0x10 },
    { 0x830, 0x88 }, { 0x950, 0x18 }, { 0x998, 0x08 },
    { 0xA14, 0x04 }, { 0xAC4, 0x34 }, { 0xB20, 0x18 },
    // bit 9: the packed word 0xA18 {M 00000601 | V 00000001}.
    // bit 10: viewData carries the atmosphere block TWICE. 0xBD0 {0.157|0.0951} and
    // 0xBE0 {1.365|4.55} are the same two pairs already seen at 0x3C0 and 0x3F0 -- mirroring
    // the first copy left the second untouched, which is why a residue survived. The camera
    // -derived neighbours 0xBA0/0xBC0/0xBFC/0xC40 stay excluded.
    { 0xA18, 0x04 }, { 0xBD0, 0x20 },
};
static const uint32_t kEnvBit[14] = { 0, 1, 2, 3, 4, 4, 5, 6, 6, 7, 7, 8, 9, 10 };


static void viewdata_fill_holes(__int64 vd) {
    const uint32_t mask = CyberpunkVR_ViewDataFixMask;
    const uint32_t fogm = CyberpunkVR_FogMirrorMask;
    if (!vd || (!mask && !fogm && !CyberpunkVR_EnvMirrorMask)
        || !g_vd_have.load(std::memory_order_acquire)) return;
    uint8_t staging[64];
    for (size_t i = 0; i < sizeof(kViewDataHoles) / sizeof(kViewDataHoles[0]); ++i) {
        if (!(mask & (1u << i))) continue;
        const ViewDataHole& h = kViewDataHoles[i];
        if (h.len > sizeof(staging)) continue;
        {
            std::lock_guard<std::mutex> lk(g_cloud_cb_mtx);
            memcpy(staging, g_vd_main + h.off, h.len);
        }
        if (viewdata_looks_like_pointer(staging, h.len)) continue;
        if (cloud_cb_raw_copy(reinterpret_cast<uint8_t*>(vd) + h.off, staging, h.len))
            InterlockedIncrement64(
                reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugViewDataFixes));
    }
    for (int k = 0; k < 3 && fogm; ++k) {
        if (!(fogm & (1u << k))) continue;
        const FogRange& f = kFogMirror[k];
        uint8_t fstage[0x90];              // the block is 144 bytes; `staging` is only 64
        if (f.len > sizeof(fstage)) continue;
        {
            std::lock_guard<std::mutex> lk(g_cloud_cb_mtx);
            memcpy(fstage, g_vd_main + f.off, f.len);
        }
        if (cloud_cb_raw_copy(reinterpret_cast<uint8_t*>(vd) + f.off, fstage, f.len))
            InterlockedIncrement64(
                reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugFogMirrors));
    }
    const uint32_t envm = CyberpunkVR_EnvMirrorMask;
    for (int k = 0; k < 14 && envm; ++k) {
        if (!(envm & (1u << kEnvBit[k]))) continue;
        const FogRange& f = kEnvMirror[k];
        uint8_t estage[0x100];
        if (f.len > sizeof(estage)) continue;
        {
            std::lock_guard<std::mutex> lk(g_cloud_cb_mtx);
            memcpy(estage, g_vd_main + f.off, f.len);
        }
        if (cloud_cb_raw_copy(reinterpret_cast<uint8_t*>(vd) + f.off, estage, f.len))
            InterlockedIncrement64(
                reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugEnvMirrors));
    }
}

// The engine's own accessor is (*wc)->vt[4](); inlined here so the fill can run from the node
// dispatch, which does not otherwise have the view object in hand.
static void viewdata_fill_from_wc(void* wc) {
    uintptr_t vd = 0;
    __try {
        const uintptr_t obj = wc ? *reinterpret_cast<uintptr_t*>(wc) : 0;
        const uintptr_t vt  = obj ? *reinterpret_cast<uintptr_t*>(obj) : 0;
        if (vt) {
            using ViewDataFn = uintptr_t(__fastcall*)(uintptr_t);
            const ViewDataFn fn = *reinterpret_cast<ViewDataFn*>(vt + 32);
            if (fn) vd = fn(obj);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (vd) viewdata_fill_holes(static_cast<__int64>(vd));
}

static __int64 __fastcall Detour_CloudCbFill(__int64 a1, __int64 a2, __int64 a3, __int64 a4,
                                             __int64 a5, int* a6, int a7, int a8) {
    const __int64 r = g_orig_cloud_cb(a1, a2, a3, a4, a5, a6, a7, a8);
    const int v = (CyberpunkVR_CloudCbMode || CyberpunkVR_ViewDataDiff)
                      ? cloud_cb_view(a3) : -1;
    if (CyberpunkVR_CloudCbMode) {
        if (v == 0)      cloud_cb_capture_main(a2);    // a2 is the constant buffer being filled
        else if (v == 1) { if (CyberpunkVR_ViewDataDiff >= 2) cloud_cb_diff_vrcam(a2);
                           cloud_cb_apply_vrcam(a2); }
    }
    if (a4 && (CyberpunkVR_ViewDataDiff || CyberpunkVR_ViewDataFixMask)) {
        const __int64 viewData = a4 - 0x430;
        __int64 ctx = 0;
        if (CyberpunkVR_ViewDataDiff) {
            __try { ctx = *reinterpret_cast<__int64*>(a3 + 0x18); }
            __except (EXCEPTION_EXECUTE_HANDLER) { ctx = 0; }
        }
        if (v == 0) {
            viewdata_capture_main(viewData);
            if (ctx) ctx_capture_main(ctx);
        } else if (v == 1 && CyberpunkVR_ViewDataDiff) {
            if (ctx) ctx_diff_vrcam(ctx);
            // The fill itself now happens at node dispatch (far earlier); by the time this runs
            // the enabled holes are already closed, so they drop out of the report.
            viewdata_diff_vrcam(viewData);
        }
    }
    return r;
}

// --- Local-shadow VSM reuse for VRCAM ------------------------------------
// CRenderNode_RenderLocalShadowMaps sub_140AD5770 renders per-light local shadow maps
// into the SHARED committed VSM atlas (Resource_26424/26425, 512x512x10 R16G16) indexed
// by a SHARED slot table: mgr = *(ctx+0x1E10), slots @ mgr+801280, count @ mgr+801292.
// VERIFIED live IDENTICAL for vrcam & main (mgr 0x179157472C0 / slots 0x175279EAD20)
// => same light->slice mapping, so reuse is index-correct. Local shadows are world-space
// per-light + temporally cached; VRCAM (fresh view) forces a full re-render (10.6x main's
// cycles) into the shared atlas -> thrashes MAIN's cache -> both re-render every frame ->
// shadow flicker + perf. Reuse = skip vrcam's node so it neither re-renders nor advances
// the shared manager; vrcam lighting samples MAIN's atlas via the shared manager. Same ABI
// as distant: a2=rdx, view ctx = *(a2+0x18), key @ ctx+0x28. If vrcam loses local shadows
// after test, the fallback is to skip only the render sub_140153260 (keep node's fg decls).
constexpr uintptr_t LOCAL_SHADOW_RVA = 0xAD5770;   // sub_140AD5770 RenderLocalShadowMaps
using LocalShadowFn = __int64(__fastcall*)(void*, void*);
static LocalShadowFn g_orig_local_shadow = nullptr;
// DEFAULT 0 since 2026-07-29. The per-node dispatch census leaves exactly one lighting-
// relevant node that never dispatches for VRCAM, and it is this one, skipped by us. It fits
// the symptom better than anything else measured: some lights work for VRCAM (near stalls)
// and some never do (street lamps, the road) -- which is what a missing shadow slice looks
// like, since a shadow-casting light with no slice is dropped rather than drawn unshadowed.
// Tested before only together with the other two reuse modes, where VRCAM re-rendering every
// local shadow (30x MAIN in the audit) could break it a second way.
// Measured 2026-07-29: with this at 0 the node does dispatch for VRCAM (AD5770 leaves the
// census) and the lamps are still unlit. Not the cause; put back for the 30x it costs.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_LocalShadowReuseMode = 1;   // 1=reuse/skip, 0=vrcam renders its own (A/B)
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugLocalShadowSkipHits = 0;

static bool local_shadow_is_vrcam(void* a2) {
    if (CyberpunkVR_LocalShadowReuseMode != 1 || !a2) return false;
    __try {
        uintptr_t ctx = *reinterpret_cast<uintptr_t*>(
            reinterpret_cast<uint8_t*>(a2) + 0x18);
        return ctx && *reinterpret_cast<uint64_t*>(ctx + 0x28) == g_vrcam_ctx_key;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static __int64 __fastcall Detour_LocalShadowMaps(void* a1, void* a2) {
    if (local_shadow_is_vrcam(a2)) { ++CyberpunkVR_DebugLocalShadowSkipHits; return 0; }
    return g_orig_local_shadow(a1, a2);
}

// --- Reflection-probe reuse for VRCAM ------------------------------------
// CRenderNode_ReflectionProbes sub_14077E610 updates the SHARED reflection-probe manager
// v4 = *(qword_143427C00 + 200) (renderer is a GLOBAL singleton, NOT per-view ctx) via
// sub_14077E030 + sub_14077FCDC. Manager is structurally view-INDEPENDENT => reuse-safe by
// construction. Probes are world-space env captures; BINDING is a separate node
// (CRenderNode_BindEnvProbes, runs for vrcam) so skipping the UPDATE for vrcam => vrcam binds
// MAIN's updated probe atlas = reuse. VRCAM does 4.3x main's probe work (redundant rebuild).
// Same ABI: a2=rdx, ctx = *(a2+0x18), key @ ctx+0x28.
constexpr uintptr_t REFLECTION_PROBES_RVA = 0x77E610;   // sub_14077E610 ReflectionProbes

// --- GI (diffuse indirect) reuse for VRCAM -------------------------------
// CRenderNode_GlobalIllumination sub_14077E664 flow (from live disasm):
//   result = sub_1401E4B60(*(a2+0x18)+0x14);            // early-out check
//   if (result) return result;                          // (jnz end)
//   renderer = *qword_143427C00; mgr = *(renderer+0xB8);
//   if (mgr) { dirty-refresh; if (bit31) sub_14077F758(mgr,a2,&camVec,..); }  // UPDATE builds
//                                                                             // SHARED 122
//   applyMgr = *(renderer+0xC0); if (applyMgr) return sub_14077E74C(applyMgr, a2); // APPLY
// Both views build the SHARED committed GI cache Resource_122 (10243 R11G11B10) -> redundant.
// Goal: vrcam should REUSE main's 122 (skip its build) but STILL apply, AND keep GIVolumes.
// Two dead ends: (1) hooking update fn sub_14077F758 -> CRASH (apply got garbage a2, minidump:
// sub_14077E74C+0x24). (2) clearing bit31 (VrcamFlagMode=3) skipped update cleanly BUT bit31
// also gates RenderGIVolumes (sub_140B779DC = interior light) -> interior went dark.
// SOLUTION: hook the GI NODE and, for vrcam, REPLICATE its flow minus the update+dirty-refresh
// (both write the shared mgr/122; main maintains them). Full control of a2 (no corruption),
// ctx bit31 untouched (GIVolumes, a SEPARATE node, still runs), apply reuses main's 122.
// RVAs verified from live disasm @ base 0x7FF6EF660000.
// ---- colour grading: give the second eye the first eye's grading source ---------------------
//
// Symptom: the scanner's green screen tint never appeared in VRCAM, though its HUD markers did.
// The capture settles where it is lost. The tonemap pass PipelineState_777 runs for BOTH views
// with an identical set of 31 root tables and both bind the SAME `3 x Texture3D<float3>` --
// three 48^3 R11G11B10 grading tables. What differs is their CONTENT at sample time: neutral for
// VRCAM, green for MAIN. They are rebuilt once per view by CRenderNode_GenerateTonemappingLUT
// (three 6x6x6 dispatches = 48^3/8^3, recorded into the ASYNC COMPUTE list -- which is why no
// census here ever saw them until compute lists started being hooked).
//
// The node's body sub_14077A36C opens with
//     viewData = sub_1401ED930(wc);  rdi = *(viewData + 0xF88);
//     if (!rdi) goto cold;           xmm6 = *(float*)(rdi + 0x44);
//     cold:  xmm6 = flt_1431EFC58;   // = -1.0f, the "unset" sentinel
// and passes xmm6 into sub_14077B538. So MAIN feeds a real grading parameter and VRCAM feeds
// "no value", which is exactly a neutral table.
//
// `viewData+0xF88` is hole bit 8 in kViewDataHoles -- it has been in that table from the start,
// labelled "composition/debug" and left off because its consumer was unknown, and the live diff
// has reported it MAIN-set/VRCAM-zero in every sample it has ever taken.
//
// Lending is scoped to this one call and restored in a __finally, so nothing is left pointing at
// another view's object. That matters: this is the same shape of edit that crashed three times
// at viewData+0x168 -- but 0x168 is a resource SET the RTT view genuinely does not own, whereas
// this is a settings object the grading code only reads one float out of.
//
// NOT tried again: skipping the node so VRCAM reuses MAIN's tables. That turns the second eye
// black. The tables are TRANSIENT -- the capture has aliasing barriers and DiscardResource
// around them, so despite sharing a resource id there is nothing of MAIN's left to sample.
// ---- what actually differs in the composed grading parameters -------------------------------
// Measured, so no more guessing at inputs: viewData+0x640 is the SAME object for both views
// (DebugGradingSrcMain == DebugGradingSrcVrcam), and lending viewData+0xF88 fired 6776 times
// and changed nothing. So watch the OUTPUT instead. sub_14077B538 fills a 40-byte parameter
// block from that shared object plus two per-view scalars:
//     void sub_14077B538(a1, viewData, a3 /*byte*/, a4 /*float*/, a5 /*out 40B*/, a6 /*out 1B*/)
//     out[0]=src[224] out[4]=src[228] out[8]=src[232] out[16]=src[236] out[20]=src[240]
//     out[28]=a3+4    out[12]=a3 ? a4 : -1.0f        out[36..39]=src[244..247]
// Snapshot it per view and print the two side by side; whatever differs is the grade.
// ---- the constant buffers the LUT build actually reads --------------------------------------
// Everything upstream measured EQUAL between the views: same shader, same three 6x6x6 dispatches,
// same grading-settings object at viewData+0x640, and a byte-identical 40-byte parameter block
// out of sub_14077B538. Yet the tables come out neutral for VRCAM and graded for MAIN. So the
// difference has to be in what PipelineState_865 itself is handed -- its constant buffers.
//
// They live in the upload ring and are bound in place, so CopyBufferRegion never sees them; the
// place they are visible is CreateConstantBufferView, which hands over the GPU address, and the
// ring is already mapped. Capture every CBV created while GenerateTonemappingLUT is on the
// stack, in creation order, per view, and diff.
static thread_local int      t_grade_cb_view = -1;   // 0 = main, 1 = vrcam, -1 = not in the node
static thread_local uint32_t t_grade_cb_idx  = 0;
constexpr uint32_t GRADE_CB_SLOTS = 8;
constexpr uint32_t GRADE_CB_MAX   = 512;
extern "C" __declspec(dllexport) int32_t CyberpunkVR_GradeCbProbe = 1;   // OFF: grading-LUT CB capture (scanner tint, parked)
static uint8_t  g_gcb[2][GRADE_CB_SLOTS][GRADE_CB_MAX];
static uint32_t g_gcb_len[2][GRADE_CB_SLOTS];
static uint32_t g_gcb_n[2] = {0, 0};
static std::mutex g_gcb_mtx;

static void grade_cb_commit(int kind, uint32_t n) {
    std::lock_guard<std::mutex> lk(g_gcb_mtx);
    g_gcb_n[kind] = n;
}

static void grade_cb_report() {
    static uint64_t s_last = 0;
    const uint64_t now = GetTickCount64();
    if (s_last && now - s_last < 5000) return;
    uint8_t a[GRADE_CB_SLOTS][GRADE_CB_MAX], b[GRADE_CB_SLOTS][GRADE_CB_MAX];
    uint32_t la[GRADE_CB_SLOTS], lb[GRADE_CB_SLOTS], na, nb;
    {
        std::lock_guard<std::mutex> lk(g_gcb_mtx);
        na = g_gcb_n[0]; nb = g_gcb_n[1];
        if (!na || !nb) return;
        memcpy(a, g_gcb[0], sizeof(a));  memcpy(b, g_gcb[1], sizeof(b));
        memcpy(la, g_gcb_len[0], sizeof(la)); memcpy(lb, g_gcb_len[1], sizeof(lb));
    }
    s_last = now;
    char line[1200];
    int u = 0;
    line[0] = 0;
    const uint32_t n = na < nb ? na : nb;
    for (uint32_t k = 0; k < n && u < static_cast<int>(sizeof(line)) - 120; ++k) {
        const uint32_t len = la[k] < lb[k] ? la[k] : lb[k];
        uint32_t diffs = 0, first = 0xFFFFFFFF;
        for (uint32_t o = 0; o + 4 <= len; o += 4)
            if (memcmp(a[k] + o, b[k] + o, 4)) { ++diffs; if (first == 0xFFFFFFFF) first = o; }
        u += snprintf(line + u, sizeof(line) - u, "cb%u(%uB/%uB d%u", k, la[k], lb[k], diffs);
        if (diffs) {
            float fm, fv;
            memcpy(&fm, a[k] + first, 4);
            memcpy(&fv, b[k] + first, 4);
            uint32_t im, iv;
            memcpy(&im, a[k] + first, 4);
            memcpy(&iv, b[k] + first, 4);
            u += snprintf(line + u, sizeof(line) - u, " @+%X M=%08X(%.5g) V=%08X(%.5g)",
                          first, im, fm, iv, fv);
        }
        u += snprintf(line + u, sizeof(line) - u, ") ");
    }
    log("[gradecb] MAIN %u cbv / VRCAM %u cbv: %s", na, nb, line);
}

// ---- the 688-byte constant block the LUT build uploads --------------------------------------
// Located in sub_14077A36C's main flow:
//     cmp   [arg_0], 12h            ; a per-view byte, from sub_1401ED918(wc)
//     mov   edi, 5256A2C8h / eax, 2D3E6FF5h ; two shader permutations
//     cmovz edi, eax                ; chosen by that byte
//     mov   ecx, 2B0h               ; 688 bytes
//     call  sub_1401EE3CC           ; <- the same uploader the cloud constants go through
// and the clouds taught us the thing that matters here: this engine puts DESCRIPTOR INDICES in
// its constant buffers (that is why mirroring the whole cloud buffer killed VRCAM's clouds --
// fields +140/+160/+164 were transient-target indices). So the choice between grading LUT
// Resource_345 (green, MAIN) and Resource_2123 (ordinary, VRCAM) is very plausibly a field in
// this block. Capture it per view and diff -- exactly the workflow that solved the clouds.
constexpr uintptr_t CB_UPLOAD_RVA   = 0x1EE3CC;
constexpr uint32_t  GRADE_CB_BYTES  = 0x2B0;      // 688
using CbUploadFn = __int64(__fastcall*)(unsigned int, void*);
static CbUploadFn g_orig_cb_upload = nullptr;
extern "C" __declspec(dllexport) int32_t CyberpunkVR_GradeUpProbe = 1;   // OFF: grading upload-ring capture (scanner tint, parked)
static uint8_t  g_gcu[2][GRADE_CB_BYTES];
static bool     g_gcu_seen[2] = {false, false};
static std::mutex g_gcu_mtx;

static bool grade_up_store(const void* src, int v) {       // SEH only, no C++ objects
    uint8_t tmp[GRADE_CB_BYTES];
    if (!cloud_cb_raw_copy(tmp, src, GRADE_CB_BYTES)) return false;
    std::memcpy(g_gcu[v], tmp, GRADE_CB_BYTES);
    g_gcu_seen[v] = true;
    return true;
}

static void grade_up_report() {
    static uint64_t s_last = 0;
    const uint64_t now = GetTickCount64();
    if (s_last && now - s_last < 5000) return;
    uint8_t a[GRADE_CB_BYTES], b[GRADE_CB_BYTES];
    {
        std::lock_guard<std::mutex> lk(g_gcu_mtx);
        if (!g_gcu_seen[0] || !g_gcu_seen[1]) return;
        std::memcpy(a, g_gcu[0], GRADE_CB_BYTES);
        std::memcpy(b, g_gcu[1], GRADE_CB_BYTES);
    }
    s_last = now;
    char line[1500];
    int u = 0, diffs = 0;
    line[0] = 0;
    for (uint32_t o = 0; o + 4 <= GRADE_CB_BYTES; o += 4) {
        uint32_t m, v;
        std::memcpy(&m, a + o, 4);
        std::memcpy(&v, b + o, 4);
        if (m == v) continue;
        ++diffs;
        float fm, fv;
        std::memcpy(&fm, &m, 4);
        std::memcpy(&fv, &v, 4);
        if (u < static_cast<int>(sizeof(line)) - 70)
            u += snprintf(line + u, sizeof(line) - u, "+%03X M=%08X(%.4g) V=%08X(%.4g)  ",
                          o, m, fm, v, fv);
    }
    log("[gradeup] 688B grading block, %d differing dwords: %s", diffs, u ? line : "(none)");
}

// Mirror selected dwords of MAIN's block into VRCAM's, scoped to the one upload.
//
// Measured, stable across every sample (the frame-to-frame noise at +0A0/+170/+178/+1D0/+198/
// +278 is transient descriptor churn and is deliberately NOT in this list, for the same reason
// mirroring the whole cloud buffer killed the clouds):
//     +0C8 / +1B0   320 vs 306   -- 2560/8 vs 2444/8, pure resolution. Never mirror.
//     +230 / +238   32330 vs 16794  -- in range for the 163840-entry descriptor heap; the
//                   prime suspect for "which grading LUT array", i.e. Resource_345 vs _2123
//     +220          exactly 176x the above -- a byte offset with the same stride
//     +248          -0.1631 vs -0.1235
//     +258          0x12 vs 0x16 -- and 0x12 is literally what `cmp [arg_0], 12h` tests before
//                   `cmovz` picks the other shader permutation
//     +1B8 / +1E0 / +270  stable, unidentified
// One bit per candidate so they can be bisected live; default is the descriptor-index pair.
static const uint32_t kGradeMirrorOff[] = {
    0x230, 0x238, 0x220, 0x248, 0x258, 0x1B8, 0x1E0, 0x270,
};
static const uint32_t kGradeMirrorCount =
    static_cast<uint32_t>(sizeof(kGradeMirrorOff) / sizeof(kGradeMirrorOff[0]));
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_GradeMirrorMask = (1u << 0) | (1u << 1);
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugGradeMirrors = 0;

static __int64 grade_up_mirror_call(unsigned int size, void* src) {   // SEH only
    uint32_t saved[16];
    const uint32_t mask = CyberpunkVR_GradeMirrorMask;
    uint8_t* p = static_cast<uint8_t*>(src);
    __try {
        for (uint32_t k = 0; k < kGradeMirrorCount; ++k) {
            if (!(mask & (1u << k))) continue;
            const uint32_t o = kGradeMirrorOff[k];
            std::memcpy(&saved[k], p + o, 4);
            std::memcpy(p + o, g_gcu[0] + o, 4);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return g_orig_cb_upload(size, src); }
    __int64 r = 0;
    __try {
        r = g_orig_cb_upload(size, src);
    } __finally {
        for (uint32_t k = 0; k < kGradeMirrorCount; ++k)
            if (mask & (1u << k)) std::memcpy(p + kGradeMirrorOff[k], &saved[k], 4);
    }
    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugGradeMirrors));
    return r;
}

static bool grade_up_is_target(unsigned int size, void* src) {
    if (size != GRADE_CB_BYTES || !src || !g_exe_base) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(g_exe_base);
    const uintptr_t work = t_current_node_work;
    return work > base && static_cast<uint32_t>(work - base) == 0xEFC110;
}

static bool grade_up_capture(void* src, int v) {
    std::lock_guard<std::mutex> lk(g_gcu_mtx);
    return grade_up_store(src, v);
}

static __int64 __fastcall Detour_CbUpload(unsigned int size, void* src) {
    if (!grade_up_is_target(size, src)) return g_orig_cb_upload(size, src);
    const int v = t_vrcam_node_active ? 1 : 0;
    if (v == 0) {                                   // MAIN: this is the reference block
        const bool ok = grade_up_capture(src, 0);
        const __int64 r = g_orig_cb_upload(size, src);
        if (ok && CyberpunkVR_GradeUpProbe) grade_up_report();
        return r;
    }
    // VRCAM: record what it WOULD have uploaded, then substitute the chosen dwords.
    if (CyberpunkVR_GradeUpProbe) grade_up_capture(src, 1);
    if (!CyberpunkVR_GradeMirrorMask || !g_gcu_seen[0]) return g_orig_cb_upload(size, src);
    return grade_up_mirror_call(size, src);
}

constexpr uintptr_t GRADING_COMPOSE_RVA = 0x77B538;
using GradingComposeFn = void (__fastcall*)(void*, void*, uint8_t, float, void*, uint8_t*);
static GradingComposeFn g_orig_grading_compose = nullptr;
extern "C" __declspec(dllexport) int32_t CyberpunkVR_GradingProbe = 1;   // OFF: composed grading block dump (scanner tint, parked)
static uint8_t  g_grade_out[2][40];
static uint8_t  g_grade_a3[2]  = {0, 0};
static float    g_grade_a4[2]  = {0.f, 0.f};
static uint8_t  g_grade_a6[2]  = {0, 0};
static bool     g_grade_seen[2] = {false, false};
static std::mutex g_grade_mtx;

static void grading_probe_report() {
    static uint64_t s_last = 0;
    const uint64_t now = GetTickCount64();
    if (s_last && now - s_last < 4000) return;
    uint8_t out[2][40], a3[2], a6[2];
    float a4[2];
    {
        std::lock_guard<std::mutex> lk(g_grade_mtx);
        if (!g_grade_seen[0] || !g_grade_seen[1]) return;
        memcpy(out, g_grade_out, sizeof(out));
        memcpy(a3, g_grade_a3, sizeof(a3));
        memcpy(a4, g_grade_a4, sizeof(a4));
        memcpy(a6, g_grade_a6, sizeof(a6));
    }
    s_last = now;
    char line[900];
    int u = 0;
    line[0] = 0;
    for (int o = 0; o < 40; o += 4) {
        uint32_t m, v;
        memcpy(&m, out[0] + o, 4);
        memcpy(&v, out[1] + o, 4);
        if (m == v) continue;
        float fm, fv;
        memcpy(&fm, &m, 4);
        memcpy(&fv, &v, 4);
        if (u < static_cast<int>(sizeof(line)) - 90)
            u += snprintf(line + u, sizeof(line) - u,
                          "+%02X M=%08X(%.5g) V=%08X(%.5g)  ", o, m, fm, v, fv);
    }
    log("[grade] a3 M=%u V=%u | a4 M=%.5g V=%.5g | a6 M=%u V=%u | differing dwords: %s",
        a3[0], a3[1], a4[0], a4[1], a6[0], a6[1], u ? line : "(none)");
}

static bool grading_snapshot(void* a5, uint8_t* a6, uint8_t a3, float a4, int v) {
    __try {
        std::lock_guard<std::mutex>* dummy = nullptr; (void)dummy;
        memcpy(g_grade_out[v], a5, 40);
        g_grade_a6[v] = a6 ? *a6 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    g_grade_a3[v] = a3;
    g_grade_a4[v] = a4;
    g_grade_seen[v] = true;
    return true;
}

static void __fastcall Detour_GradingCompose(void* a1, void* a2, uint8_t a3, float a4,
                                             void* a5, uint8_t* a6) {
    g_orig_grading_compose(a1, a2, a3, a4, a5, a6);
    if (!CyberpunkVR_GradingProbe || !a5) return;
    const int v = t_vrcam_node_active ? 1 : 0;
    {
        std::lock_guard<std::mutex> lk(g_grade_mtx);
        if (!grading_snapshot(a5, a6, a3, a4, v)) return;
    }
    grading_probe_report();
}

constexpr uintptr_t TONEMAP_LUT_RVA = 0xEFC110;   // sub_140EFC110 GenerateTonemappingLUT
// Live-settable so the field can be A/B-ed without a rebuild. 0x640 = viewData+1600, the
// grading-settings pointer sub_14077B538 reads EVERYTHING out of (+224..+248). 0xF88 was
// the first guess and is measurably not it: lending it fired 6776 times and changed
// nothing, because sub_14077B538 does `if (!v9) a4 = -1.0f` and throws that float away.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_GradingSrcOff = 0x640;
using TonemapLutFn = __int64(__fastcall*)(void*, void*);
static TonemapLutFn g_orig_tonemap_lut = nullptr;
// Parked at 0 while the composer probe says what actually differs: 0x640 is the same
// object for both views and 0xF88 is thrown away by the composer.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_GradingSrcLend = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugGradingLends = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugGradingSrcMain = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugGradingSrcVrcam = 0;
// 0 = only fill a hole (never displace), 1 = also replace a pointer the view already has.
// 0x640 IS populated for the second view -- with its own ungraded settings -- so filling alone
// would do nothing here; the whole point is to substitute, scoped to the one call.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_GradingSrcDisplace = 1;
static std::atomic<uintptr_t> g_main_grading_src{0};

// SEH only -- no C++ objects in here (C2712), and the restore has to survive an exception.
static int tonemap_view_kind(void* a2) {          // 0 = main, 1 = vrcam, -1 = other/unknown
    if (!a2) return -1;
    __try {
        uintptr_t ctx = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(a2) + 0x18);
        if (!ctx) return -1;
        const uint64_t key = *reinterpret_cast<uint64_t*>(ctx + 0x28);
        if (key == 0) return 0;
        if (key == g_vrcam_ctx_key) return 1;
        return -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Its own resolver: the shared one is declared further down, with the DrawComposition code.
using GradingViewDataFn = __int64(__fastcall*)(void*);
static GradingViewDataFn g_grading_viewdata_get = nullptr;

static uintptr_t* tonemap_grading_slot(void* a2) {
    if (!g_grading_viewdata_get && g_exe_base)
        g_grading_viewdata_get = reinterpret_cast<GradingViewDataFn>(g_exe_base + 0x1ED930);
    if (!g_grading_viewdata_get) return nullptr;
    __try {
        uint8_t* viewData = reinterpret_cast<uint8_t*>(g_grading_viewdata_get(a2));
        return viewData ? reinterpret_cast<uintptr_t*>(viewData + CyberpunkVR_GradingSrcOff) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static __int64 tonemap_call_lent(void* a1, void* a2, uintptr_t* slot, uintptr_t lend) {
    const uintptr_t saved = *slot;
    __try {
        *slot = lend;
        return g_orig_tonemap_lut(a1, a2);
    } __finally {
        *slot = saved;
    }
}

static __int64 __fastcall Detour_TonemapLut(void* a1, void* a2) {
    if (CyberpunkVR_GradeCbProbe) {
        const int kind = tonemap_view_kind(a2);
        if (kind >= 0) {
            t_grade_cb_view = kind;
            t_grade_cb_idx  = 0;
            const __int64 r = g_orig_tonemap_lut(a1, a2);
            grade_cb_commit(kind, t_grade_cb_idx);
            t_grade_cb_view = -1;
            grade_cb_report();
            return r;
        }
    }
    if (!CyberpunkVR_GradingSrcLend) return g_orig_tonemap_lut(a1, a2);
    const int kind = tonemap_view_kind(a2);
    uintptr_t* slot = (kind >= 0) ? tonemap_grading_slot(a2) : nullptr;
    if (!slot) return g_orig_tonemap_lut(a1, a2);
    if (kind == 0) {                                   // MAIN: remember where it grades from
        uintptr_t cur = 0;
        __try { cur = *slot; } __except (EXCEPTION_EXECUTE_HANDLER) { cur = 0; }
        if (cur) {
            g_main_grading_src.store(cur, std::memory_order_release);
            CyberpunkVR_DebugGradingSrcMain = cur;
        }
        return g_orig_tonemap_lut(a1, a2);
    }
    const uintptr_t lend = g_main_grading_src.load(std::memory_order_acquire);
    uintptr_t cur = 0;
    __try { cur = *slot; } __except (EXCEPTION_EXECUTE_HANDLER) { return g_orig_tonemap_lut(a1, a2); }
    CyberpunkVR_DebugGradingSrcVrcam = cur;      // so the two can be compared from outside
    if (!lend || lend == cur) return g_orig_tonemap_lut(a1, a2);
    if (cur && !CyberpunkVR_GradingSrcDisplace) return g_orig_tonemap_lut(a1, a2);
    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugGradingLends));
    return tonemap_call_lent(a1, a2, slot, lend);
}

constexpr uintptr_t GI_NODE_RVA         = 0x77E664;   // sub_14077E664 GlobalIllumination node
constexpr uintptr_t GI_EARLYCHK_RVA     = 0x1E4B60;   // sub_1401E4B60(ctx+0x14) early-out
constexpr uintptr_t GI_APPLY_RVA        = 0x77E74C;   // sub_14077E74C(applyMgr, a2) GI apply
constexpr uintptr_t RENDERER_GLOBAL_RVA = 0x3427C00;  // qword_143427C00
using GiNodeFn = char(__fastcall*)(void*, void*);
static GiNodeFn g_orig_gi_node = nullptr;
// DEFAULT 0 since 2026-07-28. A night capture (Objects/EventList_LATESTVR) shows MAIN running
// a lighting stage VRCAM does not: PSO 1030 (5424 + 4166 groups), 671 (1943 + 1636), 1316
// (16^3) and 1336 x16 over 3D grids, writing the shared GI cache Resource_122 (1024x1024x3
// R11G11B10), the 64^3 clipmap volumes Resource_3164/3166 and a 112 MB probe buffer. That is
// this hook skipping the GI build for VRCAM, and the audit agrees: GlobalIllumination is
// MAIN 0.1022 ms vs VRCAM 0.0289 ms. The reuse was measured as harmless earlier -- but in
// DAYLIGHT, where the stage is inert; at night it is what lights the street lamps.
// BACK TO 1 (2026-07-31). The 0 above was set on 2026-07-28 for the night street lamps, and
// that theory is recorded as DISPROVEN in the same hunt: all three reuse knobs off changed
// nothing for the lamps, which were finally fixed by RenderMask/DistantLights instead. What
// the 0 did keep doing is let VRCAM rebuild the SHARED GI every frame from its own frustum --
// cache Resource_122, the 64^3 clipmaps 3164/3166, the 112 MB probe buffer -- and the note at
// GI_FEATURE_BIT says exactly what that costs: "main GI (ambient light/shadows) flicker".
// It is also the ONLY reuse knob that differs from the known-good testbed snapshot of 25 Jul.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_GiReuseMode = 1;   // 0=vrcam builds its own GI, 1=reuse main's (A/B)
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugGiSkipHits = 0;

static bool gi_node_is_vrcam(void* a2) {
    if (CyberpunkVR_GiReuseMode != 1 || !a2) return false;
    __try {
        uintptr_t ctx = *reinterpret_cast<uintptr_t*>(
            reinterpret_cast<uint8_t*>(a2) + 0x18);
        return ctx && *reinterpret_cast<uint64_t*>(ctx + 0x28) == g_vrcam_ctx_key;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static char __fastcall Detour_GiNode(void* a1, void* a2) {
    if (!gi_node_is_vrcam(a2)) return g_orig_gi_node(a1, a2);
    __try {
        uintptr_t ctx = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(a2) + 0x18);
        auto earlychk = reinterpret_cast<char(__fastcall*)(void*)>(g_exe_base + GI_EARLYCHK_RVA);
        char result = earlychk(reinterpret_cast<void*>(ctx + 0x14));
        if (result) return result;                          // engine early-out
        uintptr_t renderer = *reinterpret_cast<uintptr_t*>(g_exe_base + RENDERER_GLOBAL_RVA);
        if (renderer) {
            uintptr_t applyMgr = *reinterpret_cast<uintptr_t*>(renderer + 0xC0);
            if (applyMgr) {                                 // SKIP update (122 build); apply only
                ++CyberpunkVR_DebugGiSkipHits;
                auto apply = reinterpret_cast<char(__fastcall*)(void*, void*)>(g_exe_base + GI_APPLY_RVA);
                return apply(reinterpret_cast<void*>(applyMgr), a2);
            }
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;   // safe: skip GI this frame for vrcam rather than crash
    }
}

// --- Shared culling for VRCAM (reuse main's visibility) -------------------
// CRenderNode_DoCulling (sub_140B2BEFC, gate owner-bit a2+0x30&2) snapshots a 0x48-byte
// per-view frustum descriptor (sub_1401EC7EC) and submits cull JOBS -- "DoCull_MainScene"
// (node bit1 -> sub_1406246E8) and "DoCull_Cascades" (bit50 -> sub_140C43954) -- to the
// global multi-frustum visibility system (qword_143438980, worker-pool + semaphore). The
// cull work writes visibility into the SCENE MANAGER at *(view_ctx+0x1E10), which is
// VERIFIED shared (identical ptr) between vrcam & main; occluders are gathered WORLD-SPACE.
// STEP 1 (this): skip vrcam's DoCulling entirely. If the shared manager makes vrcam's render
// reuse main's visible set -> near-free reuse (then expand main's frustum a touch to cover
// vrcam conservatively). If vrcam goes empty -> visible set is per-view-indexed -> redirect
// explicitly. Toggle LIVE via x64dbg (default OFF); watch fps + vrcam image. a2=view arg,
// ctx=*(a2+0x18), key@ctx+0x28 (same ABI as the other reuse skips).
constexpr uintptr_t DOCULLING_RVA = 0xB2BEFC;   // sub_140B2BEFC CRenderNode_DoCulling
constexpr uintptr_t MAIN_CULL_PREP_RVA = 0x62463C; // sub_14062463C scene-system candidate gather
constexpr uintptr_t MAIN_CULL_CTX_INIT_RVA = 0x623FD8; // sub_140623FD8 build shared gather-context
constexpr uintptr_t MAIN_CULL_TEST_RVA = 0x624694; // sub_140624694 -> sub_14014DBC4 tester loop
constexpr uintptr_t VIS_COLLECTOR_RVA = 0x79CB6C;  // sub_14079CB6C tagged candidates -> fresh view output
constexpr uintptr_t FINE_MATERIALIZE_RVA = 0x14DFE8; // sub_14014DFE8 per-drawable fine test/materialize
constexpr uintptr_t VISIBLE_APPEND_RVA = 0x109A44;   // sub_140109A44 append visible drawable ID
constexpr uintptr_t MATERIALIZE_WORKER_RVA = 0x36DDC4; // sub_14036DDC4 worker wrapper
constexpr uintptr_t PREPARE_STAGE_RVA = 0x1D57210;   // sub_141D57210 gather/filter/finalize wrapper
constexpr uintptr_t PREPARE_GATHER_RVA = 0x15375C;    // sub_14015375C flatten worker buckets
constexpr uintptr_t PREPARE_FILTER_RVA = 0x1D57100;   // sub_141D57100 filter/classify descriptors
constexpr uintptr_t PREPARE_FINALIZE_RVA = 0x379568;  // sub_140379568 sort/finalize descriptors
constexpr uintptr_t PREPARE_SORT_A_RVA = 0x37A54C;    // first mode1 index-domain sort
constexpr uintptr_t PREPARE_SORT_B_RVA = 0x37A984;    // second mode1 index-domain sort
constexpr uintptr_t PREPARE_SORT_C_RVA = 0x37ADB4;    // third mode1 index-domain sort
constexpr uintptr_t PREPARE_SORT_FINAL_RVA = 0x45E33C; // final 16-byte descriptor sort
using DoCullingFn = char(__fastcall*)(void*, void*, void*);
static DoCullingFn g_orig_doculling = nullptr;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_CullReuseMode = 0;   // 0=off, 1=skip VRCAM cull, 2=skip MAIN cull (diag), 4=force VRCAM query localCtx(+0x348)=0, 5=REUSE MAIN block-list v5, 6=unsafe graph-output experiment, 7=replay VRCAM tagged visibility, 8=mode7 + skip duplicate MAIN candidate gather, 9=mode8 + replay fine drawable IDs
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCullSkipHits = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugLocalCtxZeroHits = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugBlockReuseHits = 0;
// mode6: GraphContextPrepare reuses pool addresses sequentially. At entry its active list
// still names the PREVIOUS subgraph. Live order proved old=VRCAM -> GraphContextPrepare ->
// DoCulling MAIN. During that transition, preserve the entire VRCAM graph container by
// skipping sub_14079C05C (which otherwise clears both visibility buckets and payload
// metadata), then skip MAIN's duplicate cull. MAIN draw consumes VRCAM's current-frame
// output. The next MAIN->VRCAM transition performs the normal reset, so nothing accumulates.
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugContainerRedirectHits = 0;
extern "C" __declspec(dllexport) uintptr_t CyberpunkVR_DebugMainContainer = 0;
extern "C" __declspec(dllexport) uintptr_t CyberpunkVR_DebugVrcamContainer = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugVisibilityResetSkipHits = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugEndRenderResetSkipHits = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMainCullReuseHits = 0;
static std::atomic<bool> g_main_visibility_reuse_armed{false};
enum : uint32_t {
    VIS_REUSE_IDLE = 0,
    VIS_REUSE_VRCAM_ACTIVE = 1,
    VIS_REUSE_VRCAM_PRESERVED = 2,
    VIS_REUSE_MAIN = 3,
};
static std::atomic<uint32_t> g_visibility_reuse_phase{VIS_REUSE_IDLE};
static thread_local bool t_preserve_vrcam_graph = false;
static thread_local void* t_preserve_container = nullptr;

using GraphContextPrepareFn = void*(__fastcall*)(void*, void*, void*);
using GraphContextResetFn = __int64(__fastcall*)(void*);
static GraphContextPrepareFn g_orig_graph_context_prepare = nullptr;
static GraphContextResetFn g_orig_graph_context_reset = nullptr;

static void* __fastcall Detour_GraphContextPrepare(void* a1, void* a2, void* a3) {
    const bool previous_preserve = t_preserve_vrcam_graph;
    void* const previous_container = t_preserve_container;
    t_preserve_vrcam_graph = false;
    t_preserve_container = nullptr;

    if (CyberpunkVR_CullReuseMode == 6 && a1) {
        __try {
            const uint32_t count = *reinterpret_cast<uint32_t*>(
                reinterpret_cast<uint8_t*>(a1) + 0x54);
            auto** views = *reinterpret_cast<uint8_t***>(
                reinterpret_cast<uint8_t*>(a1) + 0x48);
            if (count == 1 && views && views[0]) {
                uint8_t* const old_view = views[0];
                const uint64_t key = *reinterpret_cast<uint64_t*>(old_view + 0x28);
                void* const container = *reinterpret_cast<void**>(old_view + 0x1E10);
                if (key == g_vrcam_ctx_key && container) {
                    // The prepare now rebuilding this manager is MAIN. Its reset must not
                    // destroy the VRCAM cull/draw payload we want MAIN to consume.
                    t_preserve_vrcam_graph = true;
                    t_preserve_container = container;
                    g_main_visibility_reuse_armed.store(false, std::memory_order_release);
                    CyberpunkVR_DebugVrcamContainer = reinterpret_cast<uintptr_t>(container);
                } else if (is_main_view(old_view)) {
                    // MAIN->VRCAM: allow the normal reset before VRCAM writes a fresh frame.
                    g_main_visibility_reuse_armed.store(false, std::memory_order_release);
                    g_visibility_reuse_phase.store(VIS_REUSE_IDLE, std::memory_order_release);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    } else {
        g_main_visibility_reuse_armed.store(false, std::memory_order_release);
        g_visibility_reuse_phase.store(VIS_REUSE_IDLE, std::memory_order_release);
    }

    void* const result = g_orig_graph_context_prepare(a1, a2, a3);
    t_preserve_vrcam_graph = previous_preserve;
    t_preserve_container = previous_container;
    return result;
}

static __int64 __fastcall Detour_GraphContextReset(void* container) {
    const uintptr_t return_rva = g_exe_base
        ? reinterpret_cast<uintptr_t>(_ReturnAddress()) - reinterpret_cast<uintptr_t>(g_exe_base)
        : 0;
    const uint32_t phase = g_visibility_reuse_phase.load(std::memory_order_acquire);
    // CRenderNode_EndRender -> sub_14079A804 calls reset at 0x14079A853
    // (return RVA 0x79A858). Phase identifies whether this is VRCAM or MAIN.
    const bool preserve_end_render = CyberpunkVR_CullReuseMode == 6 &&
        return_rva == 0x79A858 && phase == VIS_REUSE_VRCAM_ACTIVE;
    const bool preserve_main_prepare =
        CyberpunkVR_CullReuseMode == 6 && t_preserve_vrcam_graph &&
        container == t_preserve_container;
    if (preserve_end_render || preserve_main_prepare) {
        ++CyberpunkVR_DebugVisibilityResetSkipHits;
        if (preserve_end_render)
            ++CyberpunkVR_DebugEndRenderResetSkipHits;
        ++CyberpunkVR_DebugContainerRedirectHits; // legacy counter kept for live telemetry
        if (preserve_end_render) {
            g_visibility_reuse_phase.store(
                VIS_REUSE_VRCAM_PRESERVED, std::memory_order_release);
        } else {
            g_visibility_reuse_phase.store(VIS_REUSE_MAIN, std::memory_order_release);
            g_main_visibility_reuse_armed.store(true, std::memory_order_release);
        }
        return 0;
    }
    if (CyberpunkVR_CullReuseMode == 6 && return_rva == 0x79A858 &&
        phase == VIS_REUSE_MAIN) {
        // MAIN consumed the preserved data; its EndRender performs normal cleanup.
        g_main_visibility_reuse_armed.store(false, std::memory_order_release);
        g_visibility_reuse_phase.store(VIS_REUSE_IDLE, std::memory_order_release);
    }
    return g_orig_graph_context_reset(container);
}

// KEY TEST: visibility counts (node+0x20) are SET by cull, READ (not reset) by the
// extraction predicate sub_1402397B4. Frame order is VRCAM-first. If the counts PERSIST
// between the two views' extractions, then the SECOND view (MAIN) can skip its own cull
// and reuse the FIRST view's (VRCAM) counts -> MAIN shows the scene (mode 2). If instead
// MAIN domes, the counts are consumed/overwritten and reuse is blocked.
static bool doculling_is_vrcam(void* a2) {
    if (CyberpunkVR_CullReuseMode == 0 || !a2) return false;
    __try {
        uintptr_t ctx = *reinterpret_cast<uintptr_t*>(
            reinterpret_cast<uint8_t*>(a2) + 0x18);
        if (!ctx) return false;
        const uint64_t key = *reinterpret_cast<uint64_t*>(ctx + 0x28);
        if (CyberpunkVR_CullReuseMode == 1 || CyberpunkVR_CullReuseMode == 5)
            return key == g_vrcam_ctx_key;                       // skip VRCAM's cull
        if (CyberpunkVR_CullReuseMode == 2)                    // skip MAIN gameplay cull
            return is_main_view(reinterpret_cast<void*>(ctx)); // gameplay main only (not helpers)
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static char __fastcall Detour_DoCulling(void* a1, void* a2, void* a3) {
    // mode6: GraphContextReset was skipped only on the VRCAM->MAIN transition, so MAIN
    // sees VRCAM's complete current-frame cull output and can skip its duplicate cull.
    if (CyberpunkVR_CullReuseMode == 6 && a2) {
        __try {
            uint8_t* view = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(a2) + 0x18);
            if (view) {
                const uint64_t key = *reinterpret_cast<uint64_t*>(view + 0x28);
                const float aspect = *reinterpret_cast<float*>(view + 0x98);
                if (key == g_vrcam_ctx_key) {
                    g_visibility_reuse_phase.store(
                        VIS_REUSE_VRCAM_ACTIVE, std::memory_order_release);
                }
                if (is_main_view(view) &&
                    g_main_visibility_reuse_armed.load(std::memory_order_acquire) &&
                    g_visibility_reuse_phase.load(std::memory_order_acquire) == VIS_REUSE_MAIN) {
                    CyberpunkVR_DebugMainContainer = reinterpret_cast<uintptr_t>(
                        *reinterpret_cast<void**>(view + 0x1E10));
                    ++CyberpunkVR_DebugMainCullReuseHits;
                    ++CyberpunkVR_DebugCullSkipHits;
                    return 0;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (doculling_is_vrcam(a2)) { ++CyberpunkVR_DebugCullSkipHits; return 0; }
    return g_orig_doculling(a1, a2, a3);
}

// --- mode7: replay compact VRCAM visibility into fresh MAIN output ---------
// sub_14014DBC4 classifies global candidate records via AABB/frustum + optional HZB and
// emits batches of up to 32 tagged pointers: candidate_record | {1=intersect, 2=inside}.
// sub_14079CB6C copies those tags into an owned worker task and materializes against the
// CURRENT view context. Cache VRCAM's tags, then let MAIN run normal DoCulling + 62463C
// (fresh frame-local payload/setup) while replacing only 624694's tester-loop with replay.
struct ReplayVisibilityBatch {
    uintptr_t tags[32]{};
    uint32_t count = 0;
    uint32_t reserved = 0;
};
static_assert(offsetof(ReplayVisibilityBatch, count) == 0x100);

using MainCullTestFn = __int64(__fastcall*)(void*, void*, void*, void*, void*);
using MainCullPrepareFn = __int64(__fastcall*)(void*, void*, void*);
using MainCullCtxInitFn = void*(__fastcall*)(void*, uintptr_t, void*);
using VisibilityCollectorFn = __int64(__fastcall*)(void*, void*);
using MaterializeWorkerFn = void*(__fastcall*)(void*, void*);
using FineMaterializeFn = char(__fastcall*)(void*, __int64*, char, void*);
using VisibleAppendFn = char(__fastcall*)(void*, uintptr_t*);
static uint64_t prepare_mix64(uint64_t value);
static MainCullPrepareFn g_orig_main_cull_prepare = nullptr;
static MainCullCtxInitFn g_main_cull_ctx_init = nullptr;
static MainCullTestFn g_orig_main_cull_test = nullptr;
static VisibilityCollectorFn g_orig_visibility_collector = nullptr;
static MaterializeWorkerFn g_orig_materialize_worker = nullptr;
static FineMaterializeFn g_orig_fine_materialize = nullptr;
static VisibleAppendFn g_orig_visible_append = nullptr;
static thread_local bool t_capture_vrcam_visibility = false;
static thread_local std::vector<ReplayVisibilityBatch> t_vrcam_visibility_batches;
static std::mutex g_visibility_batches_mutex;
static std::vector<ReplayVisibilityBatch> g_vrcam_visibility_batches;
static std::atomic<uint64_t> g_vrcam_visibility_generation{0};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugVisibilityBatchCaptures = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugVisibilityBatchReplays = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugVisibilityCandidatesCaptured = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugVisibilityCandidatesReplayed = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMainCullPrepareSkips = 0;
constexpr uint32_t CULL_CALLBACK_MAX = 128;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_CullCallbackProfileEnable = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCullCallbackMethodRva[CULL_CALLBACK_MAX] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCullCallbackCallsMain[CULL_CALLBACK_MAX] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCullCallbackCallsVrcam[CULL_CALLBACK_MAX] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCullCallbackDescMain[CULL_CALLBACK_MAX] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCullCallbackDescVrcam[CULL_CALLBACK_MAX] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCullCallbackTicksMain[CULL_CALLBACK_MAX] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCullCallbackTicksVrcam[CULL_CALLBACK_MAX] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugFineCandidateCaptures = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugFineCandidateReplays = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugFineCandidateFallbacks = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugFineDrawableIdsCaptured = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugFineDrawableIdsReplayed = 0;
enum : uint32_t { FINE_REUSE_IDLE = 0, FINE_REUSE_CAPTURE = 1, FINE_REUSE_REPLAY = 2 };
static std::atomic<uint32_t> g_fine_reuse_phase{FINE_REUSE_IDLE};
static std::mutex g_fine_visibility_mutex;
static std::unordered_map<uintptr_t, std::vector<uintptr_t>> g_fine_visible_ids;
static thread_local bool t_capture_fine_ids = false;
static thread_local std::vector<uintptr_t> t_fine_ids;
static thread_local uint32_t t_materialize_view_kind = 0; // 1=VRCAM, 2=MAIN gameplay
static thread_local uintptr_t t_materialize_output_key = 0;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_MaterializeProfileEnable = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMaterializeWorkerTasksMain = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMaterializeWorkerTasksVrcam = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMaterializeTaggedMain = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMaterializeTaggedVrcam = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMaterializeFineCallsMain = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMaterializeFineCallsVrcam = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMaterializeFineRangesMain = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMaterializeFineRangesVrcam = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMaterializeDrawableAppendsMain = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMaterializeDrawableAppendsVrcam = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMaterializeRangeUniqueMain = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMaterializeRangeUniqueVrcam = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMaterializeRangeDuplicateMain = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMaterializeRangeDuplicateVrcam = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMaterializeRangeProbeOverflowMain = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMaterializeRangeProbeOverflowVrcam = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMaterializeRangeCtxUniqueMain = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMaterializeRangeCtxUniqueVrcam = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMaterializeRangeCtxDuplicateMain = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMaterializeRangeCtxDuplicateVrcam = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMaterializeRangeCtxProbeOverflowMain = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMaterializeRangeCtxProbeOverflowVrcam = 0;
constexpr uint32_t MATERIALIZE_RANGE_HASH_CAP = 1u << 22; // 4M slots, diagnostic only
static std::atomic<uint64_t> g_materialize_range_hash_main[MATERIALIZE_RANGE_HASH_CAP];
static std::atomic<uint64_t> g_materialize_range_hash_vrcam[MATERIALIZE_RANGE_HASH_CAP];
static std::atomic<uint64_t> g_materialize_range_ctx_hash_main[MATERIALIZE_RANGE_HASH_CAP];
static std::atomic<uint64_t> g_materialize_range_ctx_hash_vrcam[MATERIALIZE_RANGE_HASH_CAP];

static void materialize_prof_add(uint64_t& main_value, uint64_t& vrcam_value, uint64_t value) {
    if (t_materialize_view_kind == 1)
        InterlockedAdd64(reinterpret_cast<volatile LONG64*>(&vrcam_value), value);
    else if (t_materialize_view_kind == 2)
        InterlockedAdd64(reinterpret_cast<volatile LONG64*>(&main_value), value);
}

static void materialize_range_observe(uint64_t key_hash) {
    if (!t_materialize_view_kind || !key_hash)
        return;
    auto* table = t_materialize_view_kind == 1 ? g_materialize_range_hash_vrcam : g_materialize_range_hash_main;
    uint64_t& unique_ctr = t_materialize_view_kind == 1
        ? CyberpunkVR_DebugMaterializeRangeUniqueVrcam : CyberpunkVR_DebugMaterializeRangeUniqueMain;
    uint64_t& dup_ctr = t_materialize_view_kind == 1
        ? CyberpunkVR_DebugMaterializeRangeDuplicateVrcam : CyberpunkVR_DebugMaterializeRangeDuplicateMain;
    uint64_t& overflow_ctr = t_materialize_view_kind == 1
        ? CyberpunkVR_DebugMaterializeRangeProbeOverflowVrcam : CyberpunkVR_DebugMaterializeRangeProbeOverflowMain;
    const uint32_t start = static_cast<uint32_t>(key_hash) & (MATERIALIZE_RANGE_HASH_CAP - 1);
    for (uint32_t probe = 0; probe < 16; ++probe) {
        auto& slot = table[(start + probe) & (MATERIALIZE_RANGE_HASH_CAP - 1)];
        uint64_t cur = slot.load(std::memory_order_acquire);
        if (cur == key_hash) {
            ++dup_ctr;
            return;
        }
        if (cur == 0 && slot.compare_exchange_strong(cur, key_hash,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            ++unique_ctr;
            return;
        }
    }
    ++overflow_ctr;
}

static void materialize_range_ctx_observe(uint64_t key_hash) {
    if (!t_materialize_view_kind || !key_hash)
        return;
    auto* table = t_materialize_view_kind == 1 ? g_materialize_range_ctx_hash_vrcam : g_materialize_range_ctx_hash_main;
    uint64_t& unique_ctr = t_materialize_view_kind == 1
        ? CyberpunkVR_DebugMaterializeRangeCtxUniqueVrcam : CyberpunkVR_DebugMaterializeRangeCtxUniqueMain;
    uint64_t& dup_ctr = t_materialize_view_kind == 1
        ? CyberpunkVR_DebugMaterializeRangeCtxDuplicateVrcam : CyberpunkVR_DebugMaterializeRangeCtxDuplicateMain;
    uint64_t& overflow_ctr = t_materialize_view_kind == 1
        ? CyberpunkVR_DebugMaterializeRangeCtxProbeOverflowVrcam : CyberpunkVR_DebugMaterializeRangeCtxProbeOverflowMain;
    const uint32_t start = static_cast<uint32_t>(key_hash) & (MATERIALIZE_RANGE_HASH_CAP - 1);
    for (uint32_t probe = 0; probe < 16; ++probe) {
        auto& slot = table[(start + probe) & (MATERIALIZE_RANGE_HASH_CAP - 1)];
        uint64_t cur = slot.load(std::memory_order_acquire);
        if (cur == key_hash) {
            ++dup_ctr;
            return;
        }
        if (cur == 0 && slot.compare_exchange_strong(cur, key_hash,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            ++unique_ctr;
            return;
        }
    }
    ++overflow_ctr;
}

static void* __fastcall Detour_MaterializeWorker(void* task, void* queue_ctx) {
    if (!CyberpunkVR_MaterializeProfileEnable || !task)
        return g_orig_materialize_worker(task, queue_ctx);
    const uint32_t previous_kind = t_materialize_view_kind;
    const uintptr_t previous_output_key = t_materialize_output_key;
    t_materialize_view_kind = 0;
    t_materialize_output_key = 0;
    __try {
        auto* const view = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(task) + 24);
        if (view) {
            const uint64_t key = *reinterpret_cast<uint64_t*>(view + 0x28);
            if (key == g_vrcam_ctx_key)
                t_materialize_view_kind = 1;
            else if (is_main_view(view))
                t_materialize_view_kind = 2;
        }
        t_materialize_output_key = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(task) + 80);
        const uint32_t tagged = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(task) + 352);
        materialize_prof_add(CyberpunkVR_DebugMaterializeWorkerTasksMain,
            CyberpunkVR_DebugMaterializeWorkerTasksVrcam, 1);
        materialize_prof_add(CyberpunkVR_DebugMaterializeTaggedMain,
            CyberpunkVR_DebugMaterializeTaggedVrcam, tagged);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    void* const result = g_orig_materialize_worker(task, queue_ctx);
    t_materialize_view_kind = previous_kind;
    t_materialize_output_key = previous_output_key;
    return result;
}

// ---- LOD/detail threshold sweep: override gather-context ctx+0x28 (sub_140623FD8) ----------
// 623FD8 builds the shared gather-context; ctx+0x28 is the LOD/detail threshold (normal 1.0,
// forced 0.1 for view-kind 6/7). It flows into scene-node gather methods (vfn+248/+320/+376),
// NOT into the 14DFE8 fine test. This override sweeps the threshold live so we can measure the
// DrawableAppends delta per view. a1=ctx out, a2=view ptr, a3=cull-query. key@view+0x28,
// aspect@view+0x98. Same detection ABI as the rest of the cull hooks.
static MainCullCtxInitFn g_orig_gather_ctx_init = nullptr;
// OBSERVE-ONLY DEFAULT (2026-07-31): Enable=1 with ApplyMask=0 reads the threshold each view
// actually gets and writes nothing. Two night symptoms -- no stars, and distance lost to fog
// while MAIN is fine -- are both "a distant drawable is not there", and the note above says the
// engine FORCES this threshold to 0.1 for some view kinds instead of 1.0. If VRCAM is one of
// them it culls far geometry hard, which is exactly both symptoms. Set ApplyMask=2 and
// LodThreshValue to MAIN's reading to hand VRCAM the same threshold -- live, no rebuild.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_LodThreshOverrideEnable = 1;   // answered: detail threshold is 1.0 on both views
extern "C" __declspec(dllexport) float    CyberpunkVR_LodThreshValue = 1.0f;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_LodThreshApplyMask = 0; // b0=MAIN b1=VRCAM b2=other
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugLodThreshHitsMain = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugLodThreshHitsVrcam = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugLodThreshHitsOther = 0;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugLodThreshSeenMainBits = 0;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugLodThreshSeenVrcamBits = 0;

// One line every 5 s: what threshold each view is handed. Reported as a float because that is
// what the field is -- 1.0 is "keep everything", the smaller it gets the earlier detail drops.
static void lod_thresh_report() {
    static std::atomic<uint64_t> s_next{0};
    const uint64_t now = GetTickCount64();
    uint64_t due = s_next.load(std::memory_order_relaxed);
    if (now < due) return;
    if (!s_next.compare_exchange_strong(due, now + 5000, std::memory_order_relaxed)) return;
    const uint32_t bm = CyberpunkVR_DebugLodThreshSeenMainBits;
    const uint32_t bv = CyberpunkVR_DebugLodThreshSeenVrcamBits;
    float fm = 0.0f, fv = 0.0f;
    memcpy(&fm, &bm, 4);
    memcpy(&fv, &bv, 4);
    log("[lod] gather-ctx+0x28 detail threshold -- MAIN %.4f (0x%08X)  VRCAM %.4f (0x%08X)  "
        "applyMask=%u value=%.4f  hits M/V/other %llu/%llu/%llu",
        fm, bm, fv, bv, CyberpunkVR_LodThreshApplyMask, CyberpunkVR_LodThreshValue,
        CyberpunkVR_DebugLodThreshHitsMain, CyberpunkVR_DebugLodThreshHitsVrcam,
        CyberpunkVR_DebugLodThreshHitsOther);
}

static void* __fastcall Detour_GatherCtxInit(void* ctx, uintptr_t view, void* cull_query) {
    void* const r = g_orig_gather_ctx_init(ctx, view, cull_query);
    if (CyberpunkVR_LodThreshOverrideEnable && ctx && view) {
        __try {
            const uint64_t key = *reinterpret_cast<uint64_t*>(view + 0x28);
            const float aspect = *reinterpret_cast<float*>(view + 0x98);
            const uint32_t seen = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(ctx) + 0x28);
            uint32_t bit;
            if (key == g_vrcam_ctx_key) { bit = 2; CyberpunkVR_DebugLodThreshSeenVrcamBits = seen; }
            else if (is_main_view(reinterpret_cast<void*>(view))) { bit = 1; CyberpunkVR_DebugLodThreshSeenMainBits = seen; }
            else bit = 4;
            if (CyberpunkVR_LodThreshApplyMask & bit) {
                *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(ctx) + 0x28) = CyberpunkVR_LodThreshValue;
                if (bit == 2) ++CyberpunkVR_DebugLodThreshHitsVrcam;
                else if (bit == 1) ++CyberpunkVR_DebugLodThreshHitsMain;
                else ++CyberpunkVR_DebugLodThreshHitsOther;
            }
            lod_thresh_report();
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return r;
}

// ---- occlusion gate force ---------------------------------------------------------------
// sub_14079E50C is visQuerySingleFrustum's vfn+0x20 (per-view visibility prepare). It gates
// the whole CPU software-occlusion pipeline on ONE byte:
//     if (*(BYTE*)(this + 0x19E)) { ... sub_14079F518(this + 0x240, ...); }
// and sub_14079F518 writes both this+0x240 (occ-ctx) and this+0x240+546 == this+0x462 (flag),
// which are exactly the two fields the coarse (sub_14014DDBC) and fine (sub_14014DFE8) tests
// check before doing an occlusion query. Measured live: MAIN has 0x19E==1, VRCAM has 0.
// Forcing it to 1 makes the engine build VRCAM its OWN rasterized depth buffer from VRCAM's
// OWN frustum -> correct occlusion, no cross-eye parallax error. The occluder list itself is
// scene-wide (this+8 -> +0x80, 604 entries observed for every caller) and sub_14079F518
// already early-outs when that list is empty, so this stays safe for views without occluders.
constexpr uintptr_t VIS_QUERY_PREPARE_RVA = 0x79E50C;
constexpr uintptr_t TESTER_OCC_GATE_OFF = 0x19E;
using VisQueryPrepareFn = __int64(__fastcall*)(void*, void*);
static VisQueryPrepareFn g_orig_visquery_prepare = nullptr;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_OcclusionGateForce = 1;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugOcclGateForced = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugOcclGateAlreadyOn = 0;

static __int64 __fastcall Detour_VisQueryPrepare(void* tester, void* job) {
    if (CyberpunkVR_OcclusionGateForce && tester) {
        __try {
            auto* const gate = reinterpret_cast<uint8_t*>(tester) + TESTER_OCC_GATE_OFF;
            if (*gate) {
                ++CyberpunkVR_DebugOcclGateAlreadyOn;
            } else {
                *gate = 1;
                ++CyberpunkVR_DebugOcclGateForced;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return g_orig_visquery_prepare(tester, job);
}

static bool visibility_replay_enabled() {
    return CyberpunkVR_CullReuseMode == 7 || CyberpunkVR_CullReuseMode == 8 ||
        CyberpunkVR_CullReuseMode == 9;
}

static __int64 __fastcall Detour_MainCullPrepare(void* manager, void* job, void* output) {
    if (CyberpunkVR_CullCallbackProfileEnable && manager && job && output && g_exe_base) {
        if (!g_main_cull_ctx_init)
            g_main_cull_ctx_init = reinterpret_cast<MainCullCtxInitFn>(g_exe_base + MAIN_CULL_CTX_INIT_RVA);
        if (g_main_cull_ctx_init) {
            uint8_t* view = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(job) + 0x18);
            uint32_t view_kind = 0;
            if (view) {
                const uint64_t key = *reinterpret_cast<uint64_t*>(view + 0x28);
                if (key == g_vrcam_ctx_key)
                    view_kind = 1;
                else if (is_main_view(view))
                    view_kind = 2;
            }
            alignas(16) uint8_t gather_ctx[72] = {};
            g_main_cull_ctx_init(gather_ctx,
                *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(job) + 24), output);
            auto** callbacks = *reinterpret_cast<uintptr_t***>(reinterpret_cast<uint8_t*>(manager) + 516512);
            const uint32_t count = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(manager) + 516524);
            __int64 result = 0;
            for (uint32_t i = 0; callbacks && i < count && i < CULL_CALLBACK_MAX; ++i) {
                uintptr_t obj = reinterpret_cast<uintptr_t>(callbacks[i]);
                if (!obj) continue;
                uintptr_t method = *reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(obj) + 248);
                const uintptr_t rva = method - reinterpret_cast<uintptr_t>(g_exe_base);
                CyberpunkVR_DebugCullCallbackMethodRva[i] = rva;
                const uint32_t before = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(output) + 12);
                const int64_t t0 = prof_now();
                result = reinterpret_cast<__int64(__fastcall*)(uintptr_t, void*)>(method)(obj, gather_ctx);
                const uint64_t dt = static_cast<uint64_t>(prof_now() - t0);
                const uint32_t after = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(output) + 12);
                const uint32_t delta = after >= before ? (after - before) : 0;
                if (view_kind == 1) {
                    ++CyberpunkVR_DebugCullCallbackCallsVrcam[i];
                    CyberpunkVR_DebugCullCallbackDescVrcam[i] += delta;
                    CyberpunkVR_DebugCullCallbackTicksVrcam[i] += dt;
                } else if (view_kind == 2) {
                    ++CyberpunkVR_DebugCullCallbackCallsMain[i];
                    CyberpunkVR_DebugCullCallbackDescMain[i] += delta;
                    CyberpunkVR_DebugCullCallbackTicksMain[i] += dt;
                }
            }
            return result;
        }
    }
    if ((CyberpunkVR_CullReuseMode == 8 || CyberpunkVR_CullReuseMode == 9) && job) {
        auto* const view = *reinterpret_cast<uint8_t**>(
            reinterpret_cast<uint8_t*>(job) + 0x18);
        if (is_main_view(view)) {
            ++CyberpunkVR_DebugMainCullPrepareSkips;
            return 0;
        }
    }
    return g_orig_main_cull_prepare(manager, job, output);
}

static char __fastcall Detour_VisibleAppend(void* output, uintptr_t* drawable_id) {
    if (CyberpunkVR_MaterializeProfileEnable)
        materialize_prof_add(CyberpunkVR_DebugMaterializeDrawableAppendsMain,
            CyberpunkVR_DebugMaterializeDrawableAppendsVrcam, 1);
    if (CyberpunkVR_CullReuseMode == 9 && t_capture_fine_ids && drawable_id)
        t_fine_ids.push_back(*drawable_id);
    return g_orig_visible_append(output, drawable_id);
}

static char __fastcall Detour_FineMaterialize(
        void* tester, __int64* range, char partial, void* output) {
    if (CyberpunkVR_MaterializeProfileEnable && range && range[0]) {
        const uint64_t item_count = static_cast<uint64_t>((range[1] - range[0]) / 40);
        const uint64_t range_hash = prepare_mix64(static_cast<uint64_t>(range[0])) ^
            prepare_mix64(static_cast<uint64_t>(range[1]) + 0x9E3779B97F4A7C15ull);
        const uint64_t range_ctx_hash = range_hash ^
            prepare_mix64(t_materialize_output_key + 0xD6E8FEB86659FD93ull);
        materialize_prof_add(CyberpunkVR_DebugMaterializeFineCallsMain,
            CyberpunkVR_DebugMaterializeFineCallsVrcam, 1);
        materialize_prof_add(CyberpunkVR_DebugMaterializeFineRangesMain,
            CyberpunkVR_DebugMaterializeFineRangesVrcam, item_count);
        materialize_range_observe(range_hash);
        materialize_range_ctx_observe(range_ctx_hash);
    }
    if (CyberpunkVR_CullReuseMode != 9 || !range || !range[0])
        return g_orig_fine_materialize(tester, range, partial, output);

    const uintptr_t candidate_key = static_cast<uintptr_t>(range[0]);
    const uint32_t phase = g_fine_reuse_phase.load(std::memory_order_acquire);
    if (phase == FINE_REUSE_CAPTURE) {
        const bool previous_capture = t_capture_fine_ids;
        t_capture_fine_ids = true;
        t_fine_ids.clear();
        if (t_fine_ids.capacity() < 64)
            t_fine_ids.reserve(64);
        const char result = g_orig_fine_materialize(tester, range, partial, output);
        t_capture_fine_ids = previous_capture;
        if (!previous_capture) {
            std::lock_guard<std::mutex> lock(g_fine_visibility_mutex);
            g_fine_visible_ids[candidate_key] = t_fine_ids;
            ++CyberpunkVR_DebugFineCandidateCaptures;
            CyberpunkVR_DebugFineDrawableIdsCaptured += t_fine_ids.size();
        }
        return result;
    }

    if (phase == FINE_REUSE_REPLAY && g_orig_visible_append) {
        std::vector<uintptr_t> ids;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(g_fine_visibility_mutex);
            const auto it = g_fine_visible_ids.find(candidate_key);
            if (it != g_fine_visible_ids.end()) {
                ids = it->second;
                found = true;
            }
        }
        if (found) {
            for (uintptr_t id : ids)
                g_orig_visible_append(output, &id);
            ++CyberpunkVR_DebugFineCandidateReplays;
            CyberpunkVR_DebugFineDrawableIdsReplayed += ids.size();
            return 0;
        }
        ++CyberpunkVR_DebugFineCandidateFallbacks;
    }
    return g_orig_fine_materialize(tester, range, partial, output);
}

static __int64 __fastcall Detour_VisibilityCollector(void* context, void* batch_ptr) {
    if (visibility_replay_enabled() && t_capture_vrcam_visibility && batch_ptr) {
        auto* const batch = reinterpret_cast<ReplayVisibilityBatch*>(batch_ptr);
        const uint32_t count = (std::min)(batch->count, 32u);
        if (count) {
            ReplayVisibilityBatch cached{};
            cached.count = count;
            memcpy(cached.tags, batch->tags, sizeof(uintptr_t) * count);
            t_vrcam_visibility_batches.push_back(cached);
            CyberpunkVR_DebugVisibilityCandidatesCaptured += count;
        }
    }
    return g_orig_visibility_collector(context, batch_ptr);
}

static __int64 __fastcall Detour_MainCullTest(
        void* manager, void* job, void* output, void* tester, void* query) {
    if (!visibility_replay_enabled() || !job)
        return g_orig_main_cull_test(manager, job, output, tester, query);

    uint8_t* view = nullptr;
    uint64_t key = 0;
    float aspect = 0.0f;
    view = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(job) + 0x18);
    if (!view)
        return g_orig_main_cull_test(manager, job, output, tester, query);
    key = *reinterpret_cast<uint64_t*>(view + 0x28);
    aspect = *reinterpret_cast<float*>(view + 0x98);

    if (key == g_vrcam_ctx_key) {
        if (CyberpunkVR_CullReuseMode == 9)
            g_fine_reuse_phase.store(FINE_REUSE_CAPTURE, std::memory_order_release);
        const bool previous_capture = t_capture_vrcam_visibility;
        t_capture_vrcam_visibility = true;
        t_vrcam_visibility_batches.clear();
        if (t_vrcam_visibility_batches.capacity() < 128)
            t_vrcam_visibility_batches.reserve(128);
        const __int64 result = g_orig_main_cull_test(manager, job, output, tester, query);
        t_capture_vrcam_visibility = previous_capture;
        if (!previous_capture) {
            {
                std::lock_guard<std::mutex> lock(g_visibility_batches_mutex);
                g_vrcam_visibility_batches = t_vrcam_visibility_batches;
            }
            g_vrcam_visibility_generation.fetch_add(1, std::memory_order_release);
            ++CyberpunkVR_DebugVisibilityBatchCaptures;
        }
        return result;
    }

    if (is_main_view(view) &&
        g_vrcam_visibility_generation.load(std::memory_order_acquire) != 0 &&
        g_orig_visibility_collector) {
        if (CyberpunkVR_CullReuseMode == 9)
            g_fine_reuse_phase.store(FINE_REUSE_REPLAY, std::memory_order_release);
        std::vector<ReplayVisibilityBatch> batches;
        {
            std::lock_guard<std::mutex> lock(g_visibility_batches_mutex);
            batches = g_vrcam_visibility_batches;
        }
        if (!batches.empty()) {
            uintptr_t replay_context[5] = {
                reinterpret_cast<uintptr_t>(job),
                reinterpret_cast<uintptr_t>(output),
                reinterpret_cast<uintptr_t>(tester),
                reinterpret_cast<uintptr_t>(query),
                *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(manager) + 7224),
            };
            __int64 result = 0;
            uint64_t replayed = 0;
            for (auto& batch : batches) {
                replayed += batch.count;
                result = g_orig_visibility_collector(replay_context, &batch);
            }
            ++CyberpunkVR_DebugVisibilityBatchReplays;
            CyberpunkVR_DebugVisibilityCandidatesReplayed += replayed;
            return result;
        }
    }
    if (CyberpunkVR_CullReuseMode != 9)
        g_fine_reuse_phase.store(FINE_REUSE_IDLE, std::memory_order_release);
    return g_orig_main_cull_test(manager, job, output, tester, query);
}

// --- PrepareRenderElements stage profiler (diagnostic, default OFF) --------
using PrepareStageFn = void(__fastcall*)(void*, void*, void*, uint32_t, uint32_t);
using PrepareGatherFn = void*(__fastcall*)(void*, void*);
using PrepareFilterFn = __int64(__fastcall*)(void*, char, uint32_t, uint32_t);
using PrepareFinalizeFn = void(__fastcall*)(void*, char, __int64, __int64);
using PrepareSortFn = void(__fastcall*)(void*, void*, uint32_t, void*);
static PrepareStageFn g_orig_prepare_stage = nullptr;
static PrepareGatherFn g_orig_prepare_gather = nullptr;
static PrepareFilterFn g_orig_prepare_filter = nullptr;
static PrepareFinalizeFn g_orig_prepare_finalize = nullptr;
static PrepareSortFn g_orig_prepare_sort_a = nullptr;
static PrepareSortFn g_orig_prepare_sort_b = nullptr;
static PrepareSortFn g_orig_prepare_sort_c = nullptr;
static PrepareSortFn g_orig_prepare_sort_final = nullptr;
static thread_local uint32_t t_prepare_view_kind = 0; // 1=VRCAM, 2=MAIN gameplay
static thread_local uintptr_t t_prepare_bucket_key = 0;
static thread_local uint32_t t_prepare_stage_id = 0;
static thread_local uint8_t t_prepare_mode = 0;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_PrepareProfileEnable = 0;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_PrepareCacheAuditEnable = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareStageTicksMain = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareStageTicksVrcam = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareGatherTicksMain = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareGatherTicksVrcam = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareFilterTicksMain = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareFilterTicksVrcam = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareFinalizeTicksMain = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareFinalizeTicksVrcam = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareSortATicksMain = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareSortATicksVrcam = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareSortBTicksMain = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareSortBTicksVrcam = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareSortCTicksMain = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareSortCTicksVrcam = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareSortFinalTicksMain = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareSortFinalTicksVrcam = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareCallsMain = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareCallsVrcam = 0;
constexpr uint32_t PREPARE_STAGE_MAX = 64;
constexpr uint32_t PREPARE_MODE_MAX = 4;
constexpr uint32_t PREPARE_STAGE_MODE_COUNT = PREPARE_STAGE_MAX * PREPARE_MODE_MAX;
constexpr uint32_t PREPARE_HIST_BUCKETS = 10;
constexpr uint32_t PREPARE_TOP_BUCKETS = 24;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugPrepareTopBucketCount = 0;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugPrepareTopBucketStage[PREPARE_TOP_BUCKETS] = {};
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugPrepareTopBucketMode[PREPARE_TOP_BUCKETS] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareTopBucketTicksMain[PREPARE_TOP_BUCKETS] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareTopBucketTicksVrcam[PREPARE_TOP_BUCKETS] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareTopBucketDescMain[PREPARE_TOP_BUCKETS] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareTopBucketDescVrcam[PREPARE_TOP_BUCKETS] = {};
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugPrepareTopBucketCallsMain[PREPARE_TOP_BUCKETS] = {};
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugPrepareTopBucketCallsVrcam[PREPARE_TOP_BUCKETS] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareHistCallsMain[PREPARE_HIST_BUCKETS] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareHistCallsVrcam[PREPARE_HIST_BUCKETS] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareHistDescMain[PREPARE_HIST_BUCKETS] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareHistDescVrcam[PREPARE_HIST_BUCKETS] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareHistTicksMain[PREPARE_HIST_BUCKETS] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareHistTicksVrcam[PREPARE_HIST_BUCKETS] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareBucketCallsMain[PREPARE_STAGE_MODE_COUNT] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareBucketCallsVrcam[PREPARE_STAGE_MODE_COUNT] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareBucketDescMain[PREPARE_STAGE_MODE_COUNT] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareBucketDescVrcam[PREPARE_STAGE_MODE_COUNT] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareBucketTicksMain[PREPARE_STAGE_MODE_COUNT] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareBucketTicksVrcam[PREPARE_STAGE_MODE_COUNT] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareBucketSortATicksMain[PREPARE_STAGE_MODE_COUNT] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareBucketSortATicksVrcam[PREPARE_STAGE_MODE_COUNT] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareBucketSortBTicksMain[PREPARE_STAGE_MODE_COUNT] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareBucketSortBTicksVrcam[PREPARE_STAGE_MODE_COUNT] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareBucketSortCTicksMain[PREPARE_STAGE_MODE_COUNT] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareBucketSortCTicksVrcam[PREPARE_STAGE_MODE_COUNT] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareBucketSortFinalTicksMain[PREPARE_STAGE_MODE_COUNT] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareBucketSortFinalTicksVrcam[PREPARE_STAGE_MODE_COUNT] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareBucketCountSqMain[PREPARE_STAGE_MODE_COUNT] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareBucketCountSqVrcam[PREPARE_STAGE_MODE_COUNT] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareFilterInMain[PREPARE_STAGE_MODE_COUNT] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareFilterInVrcam[PREPARE_STAGE_MODE_COUNT] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareFilterOutMain[PREPARE_STAGE_MODE_COUNT] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareFilterOutVrcam[PREPARE_STAGE_MODE_COUNT] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareFilterCallsMain[PREPARE_STAGE_MODE_COUNT] = {};
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareFilterCallsVrcam[PREPARE_STAGE_MODE_COUNT] = {};
struct PrepareFinalizeBucketStat {
    std::atomic<uint64_t> key{0};
    std::atomic<int64_t> ticks_main{0};
    std::atomic<int64_t> ticks_vrcam{0};
    std::atomic<uint64_t> desc_main{0};
    std::atomic<uint64_t> desc_vrcam{0};
    std::atomic<uint32_t> calls_main{0};
    std::atomic<uint32_t> calls_vrcam{0};
};
static PrepareFinalizeBucketStat g_prepare_finalize_buckets[128];
static uint32_t prepare_hist_bucket(uint32_t count) {
    if (count < 64) return 0;
    if (count < 128) return 1;
    if (count < 256) return 2;
    if (count < 512) return 3;
    if (count < 1024) return 4;
    if (count < 2048) return 5;
    if (count < 4096) return 6;
    if (count < 8192) return 7;
    if (count < 16384) return 8;
    return 9;
}

static uint32_t prepare_stage_mode_index(uint32_t stage, uint8_t mode) {
    if (stage >= PREPARE_STAGE_MAX || mode >= PREPARE_MODE_MAX)
        return 0xFFFFFFFFu;
    return stage * PREPARE_MODE_MAX + mode;
}

static void prepare_bucket_add(uint64_t* main_arr, uint64_t* vrcam_arr, uint32_t index, uint64_t value) {
    if (index == 0xFFFFFFFFu)
        return;
    if (t_prepare_view_kind == 1)
        InterlockedAdd64(reinterpret_cast<volatile LONG64*>(&vrcam_arr[index]), value);
    else if (t_prepare_view_kind == 2)
        InterlockedAdd64(reinterpret_cast<volatile LONG64*>(&main_arr[index]), value);
}

static PrepareFinalizeBucketStat* prepare_finalize_bucket(uint32_t stage_id, uint8_t mode) {
    const uint64_t want = (static_cast<uint64_t>(mode) << 32) | stage_id | 1ull;
    const size_t base = static_cast<size_t>((stage_id * 131u + mode * 17u) & 127u);
    for (size_t i = 0; i < std::size(g_prepare_finalize_buckets); ++i) {
        auto& slot = g_prepare_finalize_buckets[(base + i) & 127u];
        uint64_t cur = slot.key.load(std::memory_order_acquire);
        if (cur == want)
            return &slot;
        if (cur == 0 && slot.key.compare_exchange_strong(
                cur, want, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return &slot;
        }
    }
    return nullptr;
}

extern "C" __declspec(dllexport) void CyberpunkVR_DumpPrepareFinalizeBuckets() {
    struct Row {
        uint32_t stage = 0;
        uint8_t mode = 0;
        int64_t tm = 0;
        int64_t tv = 0;
        uint64_t dm = 0;
        uint64_t dv = 0;
        uint32_t cm = 0;
        uint32_t cv = 0;
    } rows[128];
    int n = 0;
    for (auto& slot : g_prepare_finalize_buckets) {
        const uint64_t key = slot.key.load(std::memory_order_relaxed);
        if (!key)
            continue;
        auto& r = rows[n++];
        r.stage = static_cast<uint32_t>((key & 0xFFFFFFFFull) - 1ull);
        r.mode = static_cast<uint8_t>(key >> 32);
        r.tm = slot.ticks_main.exchange(0, std::memory_order_relaxed);
        r.tv = slot.ticks_vrcam.exchange(0, std::memory_order_relaxed);
        r.dm = slot.desc_main.exchange(0, std::memory_order_relaxed);
        r.dv = slot.desc_vrcam.exchange(0, std::memory_order_relaxed);
        r.cm = slot.calls_main.exchange(0, std::memory_order_relaxed);
        r.cv = slot.calls_vrcam.exchange(0, std::memory_order_relaxed);
        if (!(r.tm | r.tv | r.dm | r.dv | r.cm | r.cv))
            --n;
    }
    for (int i = 0; i < n; ++i) {
        int best = i;
        for (int j = i + 1; j < n; ++j)
            if (rows[j].tm + rows[j].tv > rows[best].tm + rows[best].tv)
                best = j;
        if (best != i) {
            Row tmp = rows[i]; rows[i] = rows[best]; rows[best] = tmp;
        }
    }
    memset(CyberpunkVR_DebugPrepareTopBucketStage, 0, sizeof(CyberpunkVR_DebugPrepareTopBucketStage));
    memset(CyberpunkVR_DebugPrepareTopBucketMode, 0, sizeof(CyberpunkVR_DebugPrepareTopBucketMode));
    memset(CyberpunkVR_DebugPrepareTopBucketTicksMain, 0, sizeof(CyberpunkVR_DebugPrepareTopBucketTicksMain));
    memset(CyberpunkVR_DebugPrepareTopBucketTicksVrcam, 0, sizeof(CyberpunkVR_DebugPrepareTopBucketTicksVrcam));
    memset(CyberpunkVR_DebugPrepareTopBucketDescMain, 0, sizeof(CyberpunkVR_DebugPrepareTopBucketDescMain));
    memset(CyberpunkVR_DebugPrepareTopBucketDescVrcam, 0, sizeof(CyberpunkVR_DebugPrepareTopBucketDescVrcam));
    memset(CyberpunkVR_DebugPrepareTopBucketCallsMain, 0, sizeof(CyberpunkVR_DebugPrepareTopBucketCallsMain));
    memset(CyberpunkVR_DebugPrepareTopBucketCallsVrcam, 0, sizeof(CyberpunkVR_DebugPrepareTopBucketCallsVrcam));
    CyberpunkVR_DebugPrepareTopBucketCount = 0;
    const int lim = n < static_cast<int>(PREPARE_TOP_BUCKETS)
        ? n : static_cast<int>(PREPARE_TOP_BUCKETS);
    for (int i = 0; i < lim; ++i) {
        CyberpunkVR_DebugPrepareTopBucketStage[i] = rows[i].stage;
        CyberpunkVR_DebugPrepareTopBucketMode[i] = rows[i].mode;
        CyberpunkVR_DebugPrepareTopBucketTicksMain[i] = static_cast<uint64_t>(rows[i].tm);
        CyberpunkVR_DebugPrepareTopBucketTicksVrcam[i] = static_cast<uint64_t>(rows[i].tv);
        CyberpunkVR_DebugPrepareTopBucketDescMain[i] = rows[i].dm;
        CyberpunkVR_DebugPrepareTopBucketDescVrcam[i] = rows[i].dv;
        CyberpunkVR_DebugPrepareTopBucketCallsMain[i] = rows[i].cm;
        CyberpunkVR_DebugPrepareTopBucketCallsVrcam[i] = rows[i].cv;
    }
    CyberpunkVR_DebugPrepareTopBucketCount = static_cast<uint32_t>(lim);
}
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareCacheHits = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareCacheMisses = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareCacheHitDescriptors = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareSetHits = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareSetMisses = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPrepareSetHitDescriptors = 0;
struct PrepareCacheAuditEntry {
    uint64_t hash = 0;
    uint64_t set_sum = 0;
    uint64_t set_xor = 0;
    uint32_t set_count = 0;
    std::vector<uint8_t> input;
};
static std::mutex g_prepare_cache_audit_mutex;
static std::unordered_map<uintptr_t, PrepareCacheAuditEntry> g_prepare_cache_audit;

static uint64_t prepare_descriptor_hash(const uint8_t* data, size_t size) {
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static uint64_t prepare_mix64(uint64_t value) {
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ull;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

static void prepare_prof_add(uint64_t& main_ticks, uint64_t& vrcam_ticks, int64_t ticks) {
    if (t_prepare_view_kind == 1) {
        InterlockedAdd64(reinterpret_cast<volatile LONG64*>(&vrcam_ticks), ticks);
    } else if (t_prepare_view_kind == 2) {
        InterlockedAdd64(reinterpret_cast<volatile LONG64*>(&main_ticks), ticks);
    }
}

static void __fastcall Detour_PrepareStage(
        void* stage_context, void* buckets, void* output, uint32_t flags0, uint32_t flags1) {
    const bool profile = CyberpunkVR_PrepareProfileEnable != 0;
    const bool cache_audit = CyberpunkVR_PrepareCacheAuditEnable != 0;
    if (!profile && !cache_audit) {
        g_orig_prepare_stage(stage_context, buckets, output, flags0, flags1);
        return;
    }
    const uint32_t previous_kind = t_prepare_view_kind;
    const uintptr_t previous_bucket_key = t_prepare_bucket_key;
    const uint32_t previous_stage_id = t_prepare_stage_id;
    const uint8_t previous_mode = t_prepare_mode;
    t_prepare_view_kind = 0;
    t_prepare_stage_id = 0;
    t_prepare_mode = 0;
    t_prepare_bucket_key = static_cast<uintptr_t>(prepare_mix64(
        reinterpret_cast<uintptr_t>(buckets)) ^
        prepare_mix64(static_cast<uint64_t>(flags0) |
            (static_cast<uint64_t>(flags1) << 32)));
    __try {
        if (stage_context) {
            t_prepare_stage_id = *reinterpret_cast<uint32_t*>(
                reinterpret_cast<uint8_t*>(stage_context) + 0x14);
            t_prepare_mode = *reinterpret_cast<uint8_t*>(
                reinterpret_cast<uint8_t*>(stage_context) + 0x18);
            const uint64_t stage_signature =
                static_cast<uint64_t>(t_prepare_stage_id) |
                (static_cast<uint64_t>(t_prepare_mode) << 32) |
                (static_cast<uint64_t>(*reinterpret_cast<uint8_t*>(
                    reinterpret_cast<uint8_t*>(stage_context) + 0x19)) << 40);
            t_prepare_bucket_key ^= static_cast<uintptr_t>(prepare_mix64(stage_signature));
        }
        auto* render_context = *reinterpret_cast<uint8_t**>(stage_context);
        auto* view = render_context
            ? *reinterpret_cast<uint8_t**>(render_context + 0x18) : nullptr;
        if (view) {
            const uint64_t key = *reinterpret_cast<uint64_t*>(view + 0x28);
            if (key == g_vrcam_ctx_key) {
                t_prepare_view_kind = 1;
            } else if (is_main_view(view)) {
                t_prepare_view_kind = 2;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    const int64_t t0 = profile ? prof_now() : 0;
    g_orig_prepare_stage(stage_context, buckets, output, flags0, flags1);
    if (profile) {
        const int64_t dt = prof_now() - t0;
        prepare_prof_add(CyberpunkVR_DebugPrepareStageTicksMain,
            CyberpunkVR_DebugPrepareStageTicksVrcam, dt);
        if (t_prepare_view_kind == 1)
            ++CyberpunkVR_DebugPrepareCallsVrcam;
        else if (t_prepare_view_kind == 2)
            ++CyberpunkVR_DebugPrepareCallsMain;
    }
    t_prepare_view_kind = previous_kind;
    t_prepare_bucket_key = previous_bucket_key;
    t_prepare_stage_id = previous_stage_id;
    t_prepare_mode = previous_mode;
}

static void* __fastcall Detour_PrepareGather(void* buckets, void* output) {
    if (!CyberpunkVR_PrepareProfileEnable || !t_prepare_view_kind)
        return g_orig_prepare_gather(buckets, output);
    const int64_t t0 = prof_now();
    void* const result = g_orig_prepare_gather(buckets, output);
    prepare_prof_add(CyberpunkVR_DebugPrepareGatherTicksMain,
        CyberpunkVR_DebugPrepareGatherTicksVrcam, prof_now() - t0);
    return result;
}

static __int64 __fastcall Detour_PrepareFilter(
        void* output, char mode, uint32_t flags0, uint32_t flags1) {
    if (!CyberpunkVR_PrepareProfileEnable || !t_prepare_view_kind)
        return g_orig_prepare_filter(output, mode, flags0, flags1);
    const uint32_t count_before = output
        ? *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(output) + 0x0C)
        : 0;
    const uint32_t bucket_index = prepare_stage_mode_index(
        t_prepare_stage_id, static_cast<uint8_t>(mode));
    prepare_bucket_add(CyberpunkVR_DebugPrepareFilterInMain,
        CyberpunkVR_DebugPrepareFilterInVrcam, bucket_index, count_before);
    prepare_bucket_add(CyberpunkVR_DebugPrepareFilterCallsMain,
        CyberpunkVR_DebugPrepareFilterCallsVrcam, bucket_index, 1);
    const int64_t t0 = prof_now();
    const __int64 result = g_orig_prepare_filter(output, mode, flags0, flags1);
    const uint32_t count_after = output
        ? *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(output) + 0x0C)
        : 0;
    prepare_bucket_add(CyberpunkVR_DebugPrepareFilterOutMain,
        CyberpunkVR_DebugPrepareFilterOutVrcam, bucket_index, count_after);
    prepare_prof_add(CyberpunkVR_DebugPrepareFilterTicksMain,
        CyberpunkVR_DebugPrepareFilterTicksVrcam, prof_now() - t0);
    return result;
}

static void __fastcall Detour_PrepareFinalize(
        void* output, char mode, __int64 a3, __int64 a4) {
    if (CyberpunkVR_PrepareCacheAuditEnable && t_prepare_view_kind == 1 && mode == 1 &&
        t_prepare_bucket_key && output) {
        const uint32_t count = *reinterpret_cast<uint32_t*>(
            reinterpret_cast<uint8_t*>(output) + 0x0C);
        auto* const data = *reinterpret_cast<uint8_t**>(output);
        if (count && data) {
            const size_t bytes = static_cast<size_t>(count) * 16;
            const uint64_t hash = prepare_descriptor_hash(data, bytes);
            uint64_t set_sum = 0;
            uint64_t set_xor = 0;
            for (uint32_t i = 0; i < count; ++i) {
                const uint64_t a = *reinterpret_cast<const uint64_t*>(data + 16ull * i);
                const uint64_t b = *reinterpret_cast<const uint64_t*>(data + 16ull * i + 8);
                const uint64_t item_hash = prepare_mix64(a) ^ prepare_mix64(b + 0x9E3779B97F4A7C15ull);
                set_sum += item_hash;
                set_xor ^= prepare_mix64(item_hash + 0xD6E8FEB86659FD93ull);
            }
            bool hit = false;
            bool set_hit = false;
            {
                std::lock_guard<std::mutex> lock(g_prepare_cache_audit_mutex);
                auto& entry = g_prepare_cache_audit[t_prepare_bucket_key];
                hit = entry.hash == hash && entry.input.size() == bytes &&
                    memcmp(entry.input.data(), data, bytes) == 0;
                set_hit = entry.set_count == count && entry.set_sum == set_sum &&
                    entry.set_xor == set_xor;
                entry.hash = hash;
                entry.set_sum = set_sum;
                entry.set_xor = set_xor;
                entry.set_count = count;
                entry.input.assign(data, data + bytes);
            }
            if (hit) {
                ++CyberpunkVR_DebugPrepareCacheHits;
                CyberpunkVR_DebugPrepareCacheHitDescriptors += count;
            } else {
                ++CyberpunkVR_DebugPrepareCacheMisses;
            }
            if (set_hit) {
                ++CyberpunkVR_DebugPrepareSetHits;
                CyberpunkVR_DebugPrepareSetHitDescriptors += count;
            } else {
                ++CyberpunkVR_DebugPrepareSetMisses;
            }
        }
    }
    if (CyberpunkVR_PrepareProfileEnable && t_prepare_view_kind && output) {
        const uint32_t count = *reinterpret_cast<uint32_t*>(
            reinterpret_cast<uint8_t*>(output) + 0x0C);
        const uint32_t bucket_index = prepare_stage_mode_index(
            t_prepare_stage_id, static_cast<uint8_t>(mode));
        const uint32_t hist = prepare_hist_bucket(count);
        prepare_bucket_add(CyberpunkVR_DebugPrepareHistCallsMain,
            CyberpunkVR_DebugPrepareHistCallsVrcam, hist, 1);
        prepare_bucket_add(CyberpunkVR_DebugPrepareHistDescMain,
            CyberpunkVR_DebugPrepareHistDescVrcam, hist, count);
        prepare_bucket_add(CyberpunkVR_DebugPrepareBucketCallsMain,
            CyberpunkVR_DebugPrepareBucketCallsVrcam, bucket_index, 1);
        prepare_bucket_add(CyberpunkVR_DebugPrepareBucketDescMain,
            CyberpunkVR_DebugPrepareBucketDescVrcam, bucket_index, count);
        prepare_bucket_add(CyberpunkVR_DebugPrepareBucketCountSqMain,
            CyberpunkVR_DebugPrepareBucketCountSqVrcam, bucket_index,
            static_cast<uint64_t>(count) * static_cast<uint64_t>(count));
        if (auto* slot = prepare_finalize_bucket(t_prepare_stage_id, static_cast<uint8_t>(mode))) {
            if (t_prepare_view_kind == 1) {
                InterlockedAdd64(reinterpret_cast<volatile LONG64*>(&slot->desc_vrcam), count);
                slot->calls_vrcam.fetch_add(1, std::memory_order_relaxed);
            } else if (t_prepare_view_kind == 2) {
                InterlockedAdd64(reinterpret_cast<volatile LONG64*>(&slot->desc_main), count);
                slot->calls_main.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    if (!CyberpunkVR_PrepareProfileEnable || !t_prepare_view_kind) {
        g_orig_prepare_finalize(output, mode, a3, a4);
        return;
    }
    const int64_t t0 = prof_now();
    g_orig_prepare_finalize(output, mode, a3, a4);
    const int64_t dt = prof_now() - t0;
    prepare_prof_add(CyberpunkVR_DebugPrepareFinalizeTicksMain,
        CyberpunkVR_DebugPrepareFinalizeTicksVrcam, dt);
    const uint32_t count = *reinterpret_cast<uint32_t*>(
        reinterpret_cast<uint8_t*>(output) + 0x0C);
    const uint32_t hist = prepare_hist_bucket(count);
    const uint32_t bucket_index = prepare_stage_mode_index(
        t_prepare_stage_id, static_cast<uint8_t>(mode));
    prepare_bucket_add(CyberpunkVR_DebugPrepareHistTicksMain,
        CyberpunkVR_DebugPrepareHistTicksVrcam, hist, static_cast<uint64_t>(dt));
    prepare_bucket_add(CyberpunkVR_DebugPrepareBucketTicksMain,
        CyberpunkVR_DebugPrepareBucketTicksVrcam, bucket_index, static_cast<uint64_t>(dt));
    if (auto* slot = prepare_finalize_bucket(t_prepare_stage_id, static_cast<uint8_t>(mode))) {
        if (t_prepare_view_kind == 1)
            InterlockedAdd64(reinterpret_cast<volatile LONG64*>(&slot->ticks_vrcam), dt);
        else if (t_prepare_view_kind == 2)
            InterlockedAdd64(reinterpret_cast<volatile LONG64*>(&slot->ticks_main), dt);
    }
}

static void __fastcall Detour_PrepareSortA(
        void* begin, void* end, uint32_t count, void* keys) {
    if (!CyberpunkVR_PrepareProfileEnable || !t_prepare_view_kind) {
        g_orig_prepare_sort_a(begin, end, count, keys);
        return;
    }
    const int64_t t0 = prof_now();
    g_orig_prepare_sort_a(begin, end, count, keys);
    const uint64_t dt = static_cast<uint64_t>(prof_now() - t0);
    prepare_prof_add(CyberpunkVR_DebugPrepareSortATicksMain,
        CyberpunkVR_DebugPrepareSortATicksVrcam, dt);
    prepare_bucket_add(CyberpunkVR_DebugPrepareBucketSortATicksMain,
        CyberpunkVR_DebugPrepareBucketSortATicksVrcam,
        prepare_stage_mode_index(t_prepare_stage_id, t_prepare_mode),
        dt);
}

static void __fastcall Detour_PrepareSortB(
        void* begin, void* end, uint32_t count, void* keys) {
    if (!CyberpunkVR_PrepareProfileEnable || !t_prepare_view_kind) {
        g_orig_prepare_sort_b(begin, end, count, keys);
        return;
    }
    const int64_t t0 = prof_now();
    g_orig_prepare_sort_b(begin, end, count, keys);
    const uint64_t dt = static_cast<uint64_t>(prof_now() - t0);
    prepare_prof_add(CyberpunkVR_DebugPrepareSortBTicksMain,
        CyberpunkVR_DebugPrepareSortBTicksVrcam, dt);
    prepare_bucket_add(CyberpunkVR_DebugPrepareBucketSortBTicksMain,
        CyberpunkVR_DebugPrepareBucketSortBTicksVrcam,
        prepare_stage_mode_index(t_prepare_stage_id, t_prepare_mode),
        dt);
}

static void __fastcall Detour_PrepareSortC(
        void* begin, void* end, uint32_t count, void* keys) {
    if (!CyberpunkVR_PrepareProfileEnable || !t_prepare_view_kind) {
        g_orig_prepare_sort_c(begin, end, count, keys);
        return;
    }
    const int64_t t0 = prof_now();
    g_orig_prepare_sort_c(begin, end, count, keys);
    const uint64_t dt = static_cast<uint64_t>(prof_now() - t0);
    prepare_prof_add(CyberpunkVR_DebugPrepareSortCTicksMain,
        CyberpunkVR_DebugPrepareSortCTicksVrcam, dt);
    prepare_bucket_add(CyberpunkVR_DebugPrepareBucketSortCTicksMain,
        CyberpunkVR_DebugPrepareBucketSortCTicksVrcam,
        prepare_stage_mode_index(t_prepare_stage_id, t_prepare_mode),
        dt);
}

static void __fastcall Detour_PrepareSortFinal(
        void* begin, void* end, uint32_t count, void* keys) {
    if (!CyberpunkVR_PrepareProfileEnable || !t_prepare_view_kind) {
        g_orig_prepare_sort_final(begin, end, count, keys);
        return;
    }
    const int64_t t0 = prof_now();
    g_orig_prepare_sort_final(begin, end, count, keys);
    const uint64_t dt = static_cast<uint64_t>(prof_now() - t0);
    prepare_prof_add(CyberpunkVR_DebugPrepareSortFinalTicksMain,
        CyberpunkVR_DebugPrepareSortFinalTicksVrcam, dt);
    prepare_bucket_add(CyberpunkVR_DebugPrepareBucketSortFinalTicksMain,
        CyberpunkVR_DebugPrepareBucketSortFinalTicksVrcam,
        prepare_stage_mode_index(t_prepare_stage_id, t_prepare_mode),
        dt);
}

// --- VRCAM localCtx test in multifrustum worker ---------------------------
// Live RE proved query+0x350 (global occlusion ctx) is ALREADY shared between MAIN and
// VRCAM. The only stable difference at worker sub_14014D03C is query+0x348:
//   MAIN  -> [query+0x348] == 0
//   VRCAM -> [query+0x348] != 0
// One-shot live nulling of VRCAM's +0x348 kept the scene intact, but FPS did not move in a
// single-frame poke. Mode 4 repeats that null EVERY invocation for a real measurement.
constexpr uintptr_t QUERYWORK_RVA = 0x14D03C;   // sub_14014D03C multifrustum worker
using QueryWorkFn = __int64(__fastcall*)(void*, void*);
static QueryWorkFn g_orig_querywork = nullptr;
static __int64 __fastcall Detour_QueryWork(void* query, void* a2) {
    if (CyberpunkVR_CullReuseMode == 4 && query) {
        void** local_ctx = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(query) + 0x348);
        void* saved = *local_ctx;
        if (saved) {
            *local_ctx = nullptr;
            ++CyberpunkVR_DebugLocalCtxZeroHits;
            __int64 r = g_orig_querywork(query, a2);
            *local_ctx = saved;
            return r;
        }
    }
    return g_orig_querywork(query, a2);
}

// --- Block-list (v5) reuse at DrawComposition layer -----------------------
// DrawComposition sub_14020A264 resolves viewData = sub_1401ED930(a2) and reads
// v5 = *(viewData + 0x168). If the scene/materialize path is enabled, it later calls
// sub_1401ECFDC(pool, v5). Live test proved: forcing VRCAM's materialize to use MAIN's
// v5 renders the SCENE correctly (no dome). The clean fix is to inject earlier: on VRCAM,
// if v5 is empty or VRCAM-like, temporarily replace viewData+0x168 with cached MAIN v5,
// call original DrawComposition, then restore. Frame order is VRCAM-first, so this uses
// MAIN's PREVIOUS-FRAME v5.
constexpr uintptr_t DRAWCOMP_RVA = 0x20A264;   // sub_14020A264
using DrawCompFn = char(__fastcall*)(void*, void*);
static DrawCompFn g_orig_drawcomp = nullptr;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugBlockV5ReuseHits = 0;
static void* g_main_block_v5 = nullptr;

// ---- lend MAIN's draw-block list to the light-volume pass -----------------------------------
// The indirect census settles what was missing: RenderLightBuffers (0x77D308) issues an indirect
// DRAW with the signature that AutoSpawnOnTerrain and RenderShadowCascade also use, and VRCAM
// issues it ZERO times -- in any node. It is not the capability gate (granted, census unchanged).
// The common cause is the one this project already recorded while chasing the HUD: the RTT view
// never collects draw blocks, so there is nothing for an indirect draw to consume. Light volumes
// are drawn geometry, which is why the sun and sky are unaffected and only local lights vanish.
//
// The fix borrows the mechanism already proven at DrawComposition (CullReuseMode 5): hand the
// view MAIN's block list for the duration of ONE call and put its own pointer back in a
// __finally. Nothing is fabricated -- this is MAIN's live pointer, and it is never left in place,
// which is what separates this from the two crashes that came of faking viewData+0x168.
constexpr uintptr_t LIGHTBUFFERS_RVA = 0x77D308;   // sub_14077D308 RenderLightBuffers
using LightBufFn = __int64(__fastcall*)(void*, void*);
static LightBufFn g_orig_lightbuffers = nullptr;
// DEFAULT 0 -- this CRASHES: EXCEPTION_ACCESS_VIOLATION reading 0x10, i.e. a null deref one
// level inside the borrowed list. Lending MAIN's real pointer scoped to a single call is
// safe at DrawComposition (CullReuseMode 5) but NOT here: RenderLightBuffers walks further
// into the block list and reaches per-view resources the RTT view does not own. That makes
// three separate crashes from writing viewData+0x168 -- treat the field as unusable and fix
// the RTT view's own block collection instead.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_LightBlockLend = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugLightBlockLends = 0;

using ViewDataGetterFn = __int64(__fastcall*)(void*);
static ViewDataGetterFn g_viewdata_get = nullptr;   // lazy init to sub_1401ED930
static __int64 __fastcall Detour_LightBuffers(void* a1, void* a2) {
    if (!CyberpunkVR_LightBlockLend || !a2 || !t_vrcam_node_active || !g_main_block_v5)
        return g_orig_lightbuffers(a1, a2);
    if (!g_viewdata_get && g_exe_base)
        g_viewdata_get = reinterpret_cast<ViewDataGetterFn>(g_exe_base + 0x1ED930);
    if (!g_viewdata_get) return g_orig_lightbuffers(a1, a2);
    void** slot = nullptr;
    void* saved = nullptr;
    __try {
        uint8_t* viewData = reinterpret_cast<uint8_t*>(g_viewdata_get(a2));
        if (viewData) {
            slot = reinterpret_cast<void**>(viewData + 0x168);
            saved = *slot;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { slot = nullptr; }
    // Only lend where the view has nothing of its own; never displace a real list.
    if (!slot || saved) return g_orig_lightbuffers(a1, a2);
    __int64 r = 0;
    __try {
        *slot = g_main_block_v5;
        InterlockedIncrement64(
            reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugLightBlockLends));
        r = g_orig_lightbuffers(a1, a2);
    } __finally {
        *slot = saved;
    }
    return r;
}

static char __fastcall Detour_DrawComposition(void* a1, void* a2) {
    if (!g_viewdata_get && g_exe_base)
        g_viewdata_get = reinterpret_cast<ViewDataGetterFn>(g_exe_base + 0x1ED930); // sub_1401ED930
    if (!a2 || !g_viewdata_get)
        return g_orig_drawcomp(a1, a2);
    __try {
        uint8_t* viewData = reinterpret_cast<uint8_t*>(g_viewdata_get(a2));
        if (!viewData)
            return g_orig_drawcomp(a1, a2);
        void** v5_slot = reinterpret_cast<void**>(viewData + 0x168);
        void* v5 = *v5_slot;
        // Live-settled discriminator at DrawComposition layer:
        //   MAIN-only  (mode1): *(DWORD*)(a2+0x14) == 0x0E
        //   VRCAM-only (mode2): *(DWORD*)(a2+0x14) == 0x0D
        const uint32_t layer_tag = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(a2) + 0x14);
        if (layer_tag == 0x0E && v5) {
            g_main_block_v5 = v5;   // cache latest MAIN block-list
            return g_orig_drawcomp(a1, a2);
        }
        if (CyberpunkVR_CullReuseMode == 5 && layer_tag == 0x0D && g_main_block_v5) {
            void* saved = *v5_slot;
            *v5_slot = g_main_block_v5;
            ++CyberpunkVR_DebugBlockV5ReuseHits;
            char r = 0;
            __try { r = g_orig_drawcomp(a1, a2); }
            __finally { *v5_slot = saved; }
            return r;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return g_orig_drawcomp(a1, a2);
}

// --- VRCAM temporal-history fix (ROOT CAUSE of no-SSR/GI/denoise) ----------
// CRenderNode_SetStreamlineConstants (sub_140788A9C) does the per-view
// current-to-history matrix copy (sub_14078933C) + Streamline constants that
// feed ALL temporal reprojection (SSR feedback, GI/SSGI history, REBLUR shadow/
// AO denoise, TAA). It is gated at entry on the view's AA/upscaler mode field:
//   obj = ([[a2]] vtable[+0x20])();  mode = *(uint32*)(obj + 0xF94);
//   proceeds only if mode == 0 (TAA) or 4 (DLSS); else EXITS before the copy.
// MAIN's view = mode 0 -> history maintained. VRCAM's RTT view = mode 1 (native/
// no-AA) -> node exits -> vrcam never accumulates any temporal history -> broken
// reflections, wrong/leaking light, triangular denoiser shadows. PROVEN live:
// forcing vrcam's mode to a temporal value makes the node run the history copy.
// Fix: MIRROR main's AA/upscaler mode onto vrcam (not hardcoded) so vrcam follows
// whatever main uses (0=TAA, 4=DLSS, ...) and DLSS parity is possible later. The
// node runs for BOTH views each frame; we observe main's mode (key==0) and apply
// it to vrcam. Frame order is VRCAM-first so vrcam uses last frame's main mode
// (mode changes rarely). Default 0 (TAA) until main is first observed.
constexpr uintptr_t SL_CONSTANTS_RVA = 0x788A9C;   // sub_140788A9C
using SlConstFn = __int64(__fastcall*)(void*, void*, void*);
static SlConstFn g_orig_sl_const = nullptr;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_StreamlineHistoryFix = 1;   // default ON
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugSlHistoryHits = 0;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugMainAaMode = 0;   // observed main AA mode  *(view+0xF94)
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugVrcamAaMode = 0;   // vrcam AA mode (pre-fix) *(view+0xF94)
// The framegraph builder-selection in sub_140219730 dispatches on the view's
// BUILD-mode = *(view+0xF90) (v93): <2 -> full gameplay (sub_141D43040, emits
// StartRender/ExtractionFinalColor/ClearFinalColorTarget/Present), ==2 HitProxy,
// ==4 -> sub_141D47FF0 (full scene, NO final-color/present), etc. This is the
// field that actually gates the "main-only" output nodes -- NOT the AA mode we
// already mirror (0xF94). Capture both live in the always-running SL hook.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugMainBuildModeF90  = 0xFFFFFFFF; // main  *(view+0xF90)
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugVrcamBuildModeF90 = 0xFFFFFFFF; // vrcam *(view+0xF90)

// ---- DLSS-for-VRCAM: give vrcam its OWN Streamline viewport ----------------
// The engine's DLSS drivers key off ONE global DLSS-state object (arg a1); the
// Streamline ViewportHandle is at a1+0x478 with the viewport id at a1+0x498 (=0
// for main). Both the constants driver (sub_14078933C -> slSetConstants) and the
// tag+eval driver (sub_141D4FDC0 -> slSetTag x N + slEvaluateFeature) submit to
// that single viewport, so vrcam (which already reaches the eval driver with its
// OWN ctx + resources -- verified live: eval a2 key == VRCAM) collides on main's
// viewport 0 and produces no distinct DLSS. Fix: when the current view is vrcam,
// flip a1+0x498 to a distinct id (default 1) around each driver call and restore
// after (idempotent, no persistent state). Streamline auto-creates a 2nd DLSS
// feature (own temporal history) for the new viewport on first eval. RVAs @ base
// 0x7FF6EF660000 (found via named refs to sl.interposer exports).
constexpr uintptr_t DLSS_EVAL_RVA  = 0x1D4FDC0;   // sub_141D4FDC0 slSetTag+slEvaluateFeature
constexpr uintptr_t DLSS_CONST_RVA = 0x78933C;    // sub_14078933C slSetConstants
constexpr uintptr_t DLSS_VP_OFF    = 0x498;       // viewport id inside DLSS-state (a1)
using DlssEvalFn  = void(__fastcall*)(void*, void*, int, int, int, int, int,
                                      int, int, int, int, int, int, int);
using DlssConstFn = __int64(__fastcall*)(void*, unsigned int);
static DlssEvalFn  g_orig_dlss_eval  = nullptr;
static DlssConstFn g_orig_dlss_const = nullptr;
// PATH-A graph-level EXPERIMENT (doc 19): the DLSS-gated frame-graph node
// sub_14292DD50 (vtable 0x14312CDA8 +0x28) inserts an extra type-4 WRITE declare of
// post-color at salt70 and does a GPU->CPU readback. It perturbs the salt70 post-color
// generation timeline so Final2D's fixed read flaps between the correct (dark) and DLSS
// (bright) versions. This flag skips the node FOR THE VRCAM VIEW ONLY, to test whether
// removing it from the salt70 timeline makes Final2D deterministically read the correct
// version. DEFAULT 0 (no effect on shipped build); reversible; SEH-guarded.
constexpr uintptr_t READBACK_NODE_RVA = 0x292DD50;   // sub_14292DD50
using ReadbackNodeFn = char(__fastcall*)(__int64, void*);
static ReadbackNodeFn g_orig_readback_node = nullptr;
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_SkipVrcamReadbackNode = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugReadbackSkips = 0;
// AUTOMATIC, NOT A SETTING. Written by Detour_FlagCompute from MAIN's own upscaler groups and
// read everywhere else; there is no overlay switch any more.
//
// It was never a real choice. Every gate that consumes it already refused to act unless MAIN had
// group 69 set, so the user-facing switch was only ever the redundant half of an AND -- and the
// half that could be wrong. On it does two things, both of which only make sense while MAIN is
// upscaling: it stands vrcam up in its own Streamline viewport, and it makes vrcam render below
// its target and upscale rather than run DLAA.
//
// Without the separate viewport the two views share viewport 0, which is not merely "vrcam gets
// no DLSS of its own" -- it actively corrupts MAIN. Both views push camera matrices and jitter
// into the same viewport and evaluate against the same temporal history, so MAIN's history is
// interleaved with frames from a camera that is somewhere else. DLSS then resolves against a
// history that keeps jumping, which reads as distant geometry shimmering under head rotation, and
// it disappears entirely with the VRCAM component off, because then nothing else touches
// viewport 0. So when MAIN is on DLSS, this being on is the correct state, not an opt-in.
//
// 0 while MAIN is not upscaling (DLAA, TAA, no upscaler, or before the first graph build) --
// which is exactly the old default, so nothing changes for those setups.
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_VrcamDlss          = 0;  // derived; do not set by hand
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_VrcamDlssViewport  = 1;  // distinct SL viewport id
// DIAGNOSTIC A/B: zero the DLSS jitter the engine feeds vrcam (live float @ a1+0x1E0/+0x1E4,
// found in x64dbg -- the source the const-setter copies into sl::Constants). If vrcam's
// GEOMETRY render is NOT jittered but DLSS un-jitters by this offset, that mismatch shimmers;
// forcing jitter=0 aligns them (stable spatial DLAA) and the flicker should stop. If the
// render IS jittered, zeroing makes it worse -> tells us jitter is not the cause.
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_VrcamDlssZeroJitter = 0;  // 1=force vrcam DLSS jitter to 0
constexpr uintptr_t DLSS_JITTER_OFF = 0x1E0;  // a1+480 (jitterX @ +0x1E0, jitterY @ +0x1E4), live-verified
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugVrcamDlssEvalHits  = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugVrcamDlssConstHits = 0;
// Set by Detour_SlConstants (camera writer) while it runs for the vrcam view, so
// the constants driver -- called INSIDE the camera writer -- knows to flip too.
static thread_local bool t_vrcam_sl_active = false;

// ApplyDLSS node work-fn (sub_14037D5C4). vrcam takes a NO-EVAL path because it lacks
// feature flag 0x45 ("DLSS eval enable"): the flag bitset is at *(ctx+0x18)+0x17D0, so
// flag 0x45 = bit (0x45&0x3F=5) of the qword at +0x17D0+(0x45>>6)*8 = +0x17D8. main has
// it set, vrcam does not (VrcamFlagMode only mirrors the FG f0/f1 word, a different set).
// Setting it for vrcam makes ApplyDLSS take the FULL eval path -> reaches the eval driver
// (verified live SAFE: reaches eval, no crash -- unlike forcing the owner bit at ctx+0x30&2
// which runs an owner-only block reading [ctx+0x1D70]+0x268 that vrcam lacks). Set before
// g_orig, restore after (minimal footprint).
constexpr uintptr_t APPLYDLSS_WORK_RVA = 0x37D5C4;   // sub_14037D5C4 (ApplyDLSS node work)
constexpr uintptr_t DLSS_FLAGSET_OFF   = 0x17D8;     // inner+0x17D0 + (0x45>>6)*8
constexpr uint64_t  DLSS_EVAL_FLAG_BIT = (1ull << 5);// flag 0x45 (bit 5 of that qword)
using ApplyDlssFn = __int64(__fastcall*)(void*, void*);
static ApplyDlssFn g_orig_applydlss = nullptr;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugVrcamApplyDlssHits = 0;

//  VRCAM POST -> CASE C (global output res) FORCE 
// Verified live (x64dbg): the view-dims getter sub_1401EDA54 returns CASE A (render
// VP+0x34=1418) for vrcam post passes (callers 772BAC/61F6D4) because ctx+0x18(VP) &&
// ctx+0x20(DRS-gate = global DRS-scaler 0x1A32..) are set. Our forced DLSS flag keeps
// vrcam DRS-active the WHOLE frame -> every resource (scene+post) gets the gate -> CASE A.
// main's POST resources have the gate clear -> CASE C -> renderer+0x148 (global output).
// FIX: hook the getter ENTRY sub_1401ED8E4 (clean 5-byte prologue, unlike the tiny-leaf
// sub_1401EDA54); for vrcam (key) in the POST phase (t_vpost, set after ApplyDLSS, cleared
// at DeclCommon/scene start) clear ctx+0x20 (gate) + ctx+0x2C (node-local dims, to skip
// CASE B) around g_orig -> getter returns CASE C = global output. In VR global == HMD
// per-eye res == vrcam output -> post lands full-res, no crop. Scene (pre-DLSS, t_vpost=0)
// keeps CASE A (render 1418) so the DLSS upscale is preserved. Save+restore so only the
// getter sees the cleared desc (caller's struct intact).
constexpr uintptr_t GETTER_RVA = 0x1ED8E4;   // sub_1401ED8E4 view-dims getter entry
using GetterFn = __int64(__fastcall*)(void*, void*);
static GetterFn g_orig_getter = nullptr;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_VrcamForceCaseC   = 0;  // vrcam post getter override: 0=off, 1=CASE C (renderer+0x148 global output; VR-correct but desktop=window res != 2444), 2=CASE B (vrcam OWN output VP+0x54=2444; matches RTT texture+DLSS out in BOTH desktop and VR -- preferred)
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugForceCaseCHits = 0;

// The eval driver's "feature changed?" check (in sub_141D4FDC0) compares the CURRENT view
// dims/ids against a cache stored in the single global DLSS-state a1 at a1+0x3C8..+0x3E7.
// With one shared state, the cache alternates between main's render res (e.g. 1114x627) and
// vrcam's (2444x2444) every frame -> "changed" always trips -> the per-viewport DLSS feature
// is RECREATED every frame -> vrcam's temporal history is reset every frame -> white flicker.
// Fix: keep a SEPARATE copy of that cache for vrcam and swap it in/out of a1 around vrcam's
// eval, so each viewport's "changed" check sees its OWN previous dims (constant) -> no false
// recreate -> stable history. (verified live: cache held main's 1114x627 while vrcam=2444.)
constexpr uintptr_t DLSS_CACHE_OFF = 0x3C8;   // a1+968 .. +0x3E7 (changed-detection cache)
constexpr size_t    DLSS_CACHE_SZ  = 32;
static uint8_t g_vrcam_dlss_cache[DLSS_CACHE_SZ] = {0};
static bool    g_vrcam_dlss_cache_valid = false;

// --- vrcam DLSS render/output dims (for the subrect+MV fix) --------------------
// Nsight proved: vrcam's OWN DLSS feature gets Render.Subrect.Dimensions and MV.Scale =
// 1114x627 (== MAIN's output 1920x1080 x scale), not vrcam's 1418x1418 (2444 x scale),
// because those derive from the SHARED DLSS-state render/output dim fields (a1+0x3D0 render,
// a1+0x3E0 output = main's), NOT from view+52/56. So DLSS reads a 1114x627 subrect of the
// square 1418^2 input -> crop. Fix: while vrcam's DLSS const/eval run, overwrite a1+0x3D0/0x3E0
// with vrcam's dims (render 1418, output 2444) so the subrect+MV become 1418. Values captured
// by Detour_RenderRes (runs earlier each frame). Gate: VrcamDlssScale (fwd-declared; defined below).
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_VrcamDlssScale;   // fwd decl (definition further down)
static volatile int32_t g_vrcam_dlss_rw = 0, g_vrcam_dlss_rh = 0;  // vrcam render dims (1418)
static volatile int32_t g_vrcam_dlss_ow = 0, g_vrcam_dlss_oh = 0;  // vrcam output dims (2444)
// POST-DLSS CROP FIX phase flag: 0 = vrcam SCENE phase (render-res 1418, DLSS input),
// 1 = vrcam POST phase (this frame's DLSS eval already ran -> remaining vrcam passes are
// OUTPUT-domain 2444). Set at end of vrcam's DLSS eval; reset each frame at main's ApplyDLSS
// (which runs once per frame, before the vrcam mirror view, by frame-graph dependency).
// Root cause (verified live): the render-res writer sub_1404E42A0 writes the render-res sub-
// struct (VP+0x34 renderW / VP+0x38 renderH + copies) that ALL of a view's raster passes read
// for their D3D viewport (via sub_1401F6350 -> RSSetViewports). We force the DLSS flag ON for
// the WHOLE vrcam frame (needed for anti-flicker source-selection), so g_orig scales that field
// to 1418 on EVERY writer call -> even post-DLSS passes get a 1418 viewport on the 2444 output
// = crop. MAIN never scales its post passes (its post-DLSS passes are output-domain). Fix:
// decouple the flag from the render-res VALUE -- keep the flag set, but in the post phase make
// our own override write the TARGET (2444) instead of the scaled 1418.
// (removed: dead vrcam_dlss_force_dims / vrcam_dlss_restore_dims + DLSS_*_DIMS_OFF + g_vrcam_eval_done.
//  The DLSS-subrect force was never called; vrcam's DLSS subrect is correct natively -- see the
//  "ATTEMPT 3 DISPROVEN" note above. g_vrcam_dlss_rw/rh/ow/oh remain, published by Detour_RenderRes.)

// getter-entry hook: force vrcam POST resources to output-domain (see block above).
//   mode 1 = CASE C: clear gate + node dims -> getter falls through to renderer+0x148
//            (global output). VR-correct (global == HMD per-eye == 2444) but on DESKTOP
//            global == window res (1920) != 2444 texture -> 3-res mismatch artifacts.
//   mode 2 = CASE B: clear gate, but SET node dims (ctx+0x2C word=W, +0x2E word=H) to
//            vrcam's OWN output (VP+0x54 / VP+0x58 = 2444) -> getter returns 2444 directly,
//            matching the DLSS output + RTT texture in BOTH desktop and VR (no global dep).

// ===== VRCAM MIRROR OUTPUT ==================================================
// Standalone borderless window + own D3D12 swapchain that mirrors the vrcam RTT
// texture every frame (CopyResource -> our backbuffer -> Present). Capture in
// OBS via Window/Game Capture. Fully toggle-gated + self-disabling on any error
// so it can never take down the game. v1: quality/sync refined later.
// Default OFF: the mirror is a debug/streaming view, not part of the VR path, and it costs a
// second swapchain + a per-frame copy. Turned on from the overlay ("VRCAM Mirror"). Safe to
// flip at any time: the present thread is started lazily from the vrcam blit submit, so with
// this at 0 it simply never starts, and switching to 1 later starts it on the next vrcam frame.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_MirrorOutput      = 0;  // D3D11On12 mirror (OBS-style, non-blocking)
// State the vrcam DynamicTexture (CopyToTexture output) rests in when our copy runs.
// Per rt_dump.h prior knowledge: the RTT DynamicTexture rests in PIXEL_SHADER_RESOURCE
// (0x80=128), NOT RENDER_TARGET (4) -- copying from the wrong state => black. Runtime
// tunable so it can be A/B'd live via x64dbg (0=COMMON, 4=RT, 64=NON_PS, 128=PS_RES).
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_MirrorCopyState  = 64;  // NON_PIXEL_SHADER_RESOURCE: fixed GUESS fallback only
// The vrcam RenderFinal2D output's resting D3D12 state VARIES frame-to-frame. A fixed
// guess (CyberpunkVR_MirrorCopyState) makes the copy's transition barrier StateBefore
// wrong on mismatched frames -> hazard -> the copy reads stale/aliased heap memory
// (main's content, since vrcam's output shares the transient heap with main) -> the
// bright/dark alternation. hk_ResourceBarrier already tracks the resource's ACTUAL
// state (CyberpunkVR_DebugMirrorSrcState); use THAT for the copy barrier. 1=track (fix),
// 0=old fixed-guess behavior (for A/B).
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_MirrorTrackState = 1;
// Redirect the vrcam RenderFinal2D output RTV to our OWN committed target so the engine
// renders the final into a stable, never-aliased resource we control. View-aware (only
// inside the ctx-keyed vrcam RenderFinal2D node); no resolution heuristic -> VR-safe.
// Default 0 (enable live via x64dbg after deploy, so a bad build can't wedge startup).
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_VrcamOwnTarget    = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugOwnTargetSubs = 0;
// Diagnostic: when 1, the present thread clears the mirror window to RED and does
// NOT copy mtex. Red window => present/window pipeline works => black is the source
// copy. Black window => present/window itself is broken. Isolates the two halves.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_MirrorTestPattern = 0;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugMirrorState  = 0;  // 0 idle 1 finding 2 init 3 running 9 failed
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMirrorRes    = 0;  // found vrcam ID3D12Resource
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugMirrorW      = 0;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugMirrorH      = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMirrorFrames = 0;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugMirrorLastHr = 0;

static std::atomic<bool> g_mirror_thread_started{false};




// THE __except HERE IS NOT A SAFETY NET, AND A CRASH DUMP PROVED IT.
//
//     EXCEPTION 0xC0000005  read at FFFFFFFFFFFFFFFF
//     RIP  CyberpunkVR_Stereo.dll +0x459AF     mov eax,[rcx+40h]     <- this line, rcx from +0x1E8
//     rcx  3E7DB6CC46CD6766
//
// The component pointer was stale -- a frame-graph rebuild had freed it -- so +0x1E8 read whatever
// the allocator left behind, the null test passed, and the dereference went to a NON-CANONICAL
// address. That is a #GP, not a page fault, which is why Windows reports the address as -1 and why
// no CR2 appears. The game's own vectored crash handler runs before any frame-based __except, so
// this handler never got the chance it was written for: the process was already writing a dump.
//
// Hence a plausibility test in front of every dereference. A live heap pointer here is canonical,
// above the first 64 KB, and 8-aligned; 3E7DB6CC46CD6766 fails the first of those outright. This is
// cheap, it runs before anything can fault, and it does not depend on being allowed to handle an
// exception that the host may claim first.
static inline bool mirror_ptr_plausible(uintptr_t p) {
    return p >= 0x10000u && p < 0x00007FFFFFFFFFFFull && (p & 7u) == 0;
}
static bool mirror_rd_dtex(uintptr_t comp, uintptr_t* dtex, UINT* w, UINT* h) {
    if (!mirror_ptr_plausible(comp)) return false;
    __try {
        uintptr_t d = *reinterpret_cast<uintptr_t*>(comp + 0x1E8);
        if (d && !mirror_ptr_plausible(d)) return false;   // freed component, garbage in the slot
        *dtex = d;
        if (d) { *w = *reinterpret_cast<UINT*>(d + 0x40); *h = *reinterpret_cast<UINT*>(d + 0x44); }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// BFS from the RTT dtex object: scan each object's first 0x400 bytes for a
// D3D12 TEXTURE2D whose dims match the dtex (vrcam output); enqueue heap ptrs.

// ---- Capture the persistent vrcam output -----------------------------------
// The cooked dtex is an R8G8B8A8_UNORM resource with an SRGB RTV. The old
// dummy-device copy hook rejected UNORM and is incompatible with SL's D3D12
// interposer. Hooks below are installed on the real game device instead.
static std::atomic<ID3D12Resource*> g_captured_vrcam_res{nullptr};
static UINT g_target_w = 0, g_target_h = 0;

extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugMirrorFmt = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMirrorStage = 0;

static std::mutex g_mirror_resource_mtx;

// ---- D3D12 decoupled mirror (real second swapchain for VRCAM) --------------
// GAME thread appends ONE CopyResource(dtex -> g_d12_mtex) into the game's OWN
// blit command list (at the dtex's RENDER_TARGET->read barrier, so state is exact
// and there is no extra queue / allocator / cross-API sync). A dedicated thread
// owns its own command queue + D3D12 swapchain + window (message pump) and copies
// g_d12_mtex into its backbuffer and Presents -> a genuine capturable backbuffer,
// zero present cost on the game thread. (globals declared earlier.)
static bool d12_mirror_ensure(const D3D12_RESOURCE_DESC& src) {
    if (g_d12_mtex) return true;
    std::lock_guard<std::mutex> lk(g_d12_mtx);
    if (g_d12_mtex) return true;
    if (!g_game_device) return false;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
    // ALLOW_RENDER_TARGET so the HUD debug overlay can be drawn straight onto the mirror image
    // (see hud_mirror_overlay). This is NOT the flag that broke 11on12 sharing before -- that was
    // ALLOW_SIMULTANEOUS_ACCESS; a wrapped resource being a render target is ordinary.
    D3D12_RESOURCE_DESC d = src;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource* tex = nullptr;
    if (FAILED(g_game_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&tex))) || !tex) {
        // Fall back to the historical plain target: the mirror is worth more than the overlay.
        d.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(g_game_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&tex))) || !tex) return false;
        log("[mirror] mirror-tex has no RENDER_TARGET flag -- HUD debug overlay disabled");
    } else {
        g_d12_mtex_is_rt = true;
    }
    if (!g_d12_fence && FAILED(g_game_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&g_d12_fence)))) { tex->Release(); return false; }
    tex->SetName(L"CyberpunkVR_MirrorTex");
    g_d12_w = (UINT)d.Width; g_d12_h = d.Height; g_d12_fmt = d.Format;
    g_d12_mtex = tex;
    CyberpunkVR_DebugMirrorStage = reinterpret_cast<uint64_t>(tex);
    log("[mirror] d12 mirror-tex=%p %ux%u fmt=%u", tex, g_d12_w, g_d12_h, (unsigned)g_d12_fmt);
    return true;
}

static bool mirror_rgba8_fmt(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM
        || f == DXGI_FORMAT_B8G8R8A8_UNORM
        || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
        || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
        || f == DXGI_FORMAT_R8G8B8A8_TYPELESS
        || f == DXGI_FORMAT_B8G8R8A8_TYPELESS
        // vrcam final RenderFinal2D output is an HDR packed float target:
        || f == DXGI_FORMAT_R11G11B10_FLOAT
        || f == DXGI_FORMAT_R16G16B16A16_FLOAT
        || f == DXGI_FORMAT_R10G10B10A2_UNORM;
}

static bool mirror_get_resource_desc(ID3D12Resource* resource,
        D3D12_RESOURCE_DESC* desc) {
    if (!resource || !desc) return false;
    __try {
        *desc = resource->GetDesc();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool mirror_target_dimensions(const D3D12_RESOURCE_DESC& desc) {
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return false;
    // PRIMARY source: the resolution encoded in the SELECTED component's name. This used to
    // fall back to a literal 2444x2444, which silently killed the mirror at every other
    // resolution: no render target ever matched, so no RTV was ever registered as a candidate,
    // so the vrcam target was never captured and the present thread waited forever for a
    // texture that was never created. Nothing downstream logged anything, because nothing
    // downstream ever ran.
    UINT tw = g_vrcam_sel_w.load(std::memory_order_relaxed);
    UINT th = g_vrcam_sel_h.load(std::memory_order_relaxed);
    if (!tw || !th) {
        // Fallback: read the dims off the live RTT component, when we have its address.
        if (!g_target_w) {
            uintptr_t comp = static_cast<uintptr_t>(CyberpunkVR_DebugRttComp);
            uintptr_t dtex = 0; UINT w = 0, h = 0;
            if (comp && mirror_rd_dtex(comp, &dtex, &w, &h) && w && h) {
                g_target_w = w;
                g_target_h = h;
            }
        }
        tw = g_target_w; th = g_target_h;
    }
    if (!tw || !th) {
        static bool warned = false;          // loud once, instead of matching a wrong size
        if (!warned) {
            warned = true;
            log("[mirror] no VRCAM resolution known (component=%s) -> RTV capture disabled",
                g_vrcam_component);
        }
        return false;
    }
    // ONE ACCEPTED SIZE, THE ONE FROM THE COMPONENT NAME. Nothing else.
    //
    // 0.2.2 added a second answer here -- the dtex dims read live off the RTT component -- to fix
    // the eye going mono after the inventory. It fixed nothing and broke two things, and all three
    // facts are measured rather than argued:
    //
    //   * It was never needed. The RTT stayed 3072x3072 for entire sessions, exactly what the name
    //     vrcam_3072x3072 parses to, so this predicate had not rejected a single target.
    //   * "Purely additive" was true of the RETURN VALUE and false of everything downstream. This
    //     predicate also gates REGISTRATION, so when the live read came back 1x5 every 1x5 RGBA8
    //     target in the engine became a vrcam-output candidate. Forty a second poured into the
    //     512-slot table, the cursor wrapped, the one entry that mattered was evicted, and the
    //     second eye went mono -- the symptom it was written to cure.
    //   * The read itself killed the process. A frame-graph rebuild frees the component, so
    //     CyberpunkVR_DebugRttComp dangles; +0x1E8 then holds allocator debris, and following it
    //     landed on a non-canonical address. That is the 0xC0000005-at-FFFFFFFFFFFFFFFF dump, and
    //     the 1x5 was the same debris on a luckier frame.
    //
    // So it is gone rather than patched. A guess that has to be sanity-checked against garbage it
    // should not be reading is not a fallback. If a rebuild ever does move the RTT for real, the
    // fix belongs where the size is CHOSEN -- re-derived from the component the way the name is
    // parsed -- not in a per-bind predicate following a pointer nobody owns.
    return (UINT)desc.Width == tw && desc.Height == th;
}

// The vrcam outputs that have actually been published, readable without taking a lock so the
// eviction picker below can consult it on the descriptor-creation path. Append-only and the
// entries are AddRef'd for the session, so a relaxed scan is safe: a pointer read here is either
// null or a resource that outlives the read.
static std::array<std::atomic<ID3D12Resource*>, 64> g_mirror_pinned{};
static std::atomic<uint32_t> g_mirror_pinned_n{0};
static bool mirror_is_pinned_output(ID3D12Resource* r) {
    if (!r) return false;
    uint32_t n = g_mirror_pinned_n.load(std::memory_order_acquire);
    if (n > g_mirror_pinned.size()) n = (uint32_t)g_mirror_pinned.size();
    for (uint32_t i = 0; i < n; ++i)
        if (g_mirror_pinned[i].load(std::memory_order_relaxed) == r) return true;
    return false;
}

struct MirrorRtvCandidate {
    SIZE_T handle = 0;
    ID3D12Resource* resource = nullptr;
    DXGI_FORMAT resource_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN;
};
// Sized for BOTH views: in VR, MAIN renders at the same resolution as VRCAM, so its render
// targets pass the same coarse dimension filter and land here too. That is harmless for
// correctness -- the actual VRCAM discrimination is the node gate in hk_OMSetRenderTargets,
// which only captures while the vrcam RenderFinal2D node is on the stack -- but with a small
// table the vrcam entry could be refused once MAIN had filled it. Hence the ring below:
// running out of slots must never be able to drop the one target we need.
static std::array<MirrorRtvCandidate, 512> g_mirror_rtv_candidates{};
static std::atomic<uint32_t> g_mirror_rtv_candidate_count{0};
static std::atomic<uint32_t> g_mirror_rtv_next{0};      // ring cursor once full
static std::mutex g_mirror_rtv_candidate_mtx;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMirrorRtvRegs = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugMirrorRtvEvicts = 0;
// Countdown of RTV binds inside the vrcam node still to be logged (see the [rtvpick]
// diagnostic in hk_OMSetRenderTargets). Burns down to 0 so it costs nothing after the
// first frames; raise it live from the debugger to sample again.
extern "C" __declspec(dllexport) int32_t CyberpunkVR_DebugRtvPickLog = 48;

// ---- resources owned by US, not by the engine ---------------------------------------
// The mirror recognises the VRCAM render target by shape alone: an RGBA8-family (or
// packed-HDR) 2D texture whose size equals the selected VRCAM resolution. That was
// unambiguous while the engine was the only thing creating such textures.
//
// It stopped being unambiguous once the OpenXR submit path came in: its per-eye capture
// textures are deliberately the SAME size as VRCAM, in an RGBA8 family, and eye 1 gets a
// render-target view (the sRGB encode blit). So they matched the heuristic, got
// registered as VRCAM candidates, and the mirror started sampling OUR half-written
// capture instead of the engine's output -- which is exactly the "left side fine, right
// side a different pass, bottom-right black" corruption, visible in the desktop mirror
// because the fault is upstream of any submit.
//
// Anything we create ourselves registers here and is skipped by the heuristics.
static std::array<std::atomic<ID3D12Resource*>, 32> g_foreign_res{};

extern "C" __declspec(dllexport) void CyberpunkVR_RegisterForeignResource(ID3D12Resource* r) {
    if (!r) return;
    for (auto& slot : g_foreign_res) {
        ID3D12Resource* cur = slot.load(std::memory_order_acquire);
        if (cur == r) return;                       // already known
        if (cur) continue;
        ID3D12Resource* expected = nullptr;
        if (slot.compare_exchange_strong(expected, r, std::memory_order_acq_rel)) {
            log("[mirror] foreign resource registered %p (excluded from VRCAM detection)", r);
            return;
        }
    }
    // Full table: log rather than silently letting the next one through, because a miss
    // here shows up as mirror/eye corruption and would be maddening to trace.
    log("[mirror] WARNING foreign-resource table full, %p NOT excluded", r);
}

static bool mirror_is_foreign(ID3D12Resource* r) {
    if (!r) return false;
    for (const auto& slot : g_foreign_res) {
        ID3D12Resource* cur = slot.load(std::memory_order_acquire);
        if (!cur) break;                            // filled in order; first null ends it
        if (cur == r) return true;
    }
    return false;
}

static void mirror_register_rtv(ID3D12Resource* resource, DXGI_FORMAT view_format,
        D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    D3D12_RESOURCE_DESC desc{};
    if (!handle.ptr || mirror_is_foreign(resource) || !mirror_rgba8_fmt(view_format) ||
        !mirror_get_resource_desc(resource, &desc) ||
        !mirror_rgba8_fmt(desc.Format) || !mirror_target_dimensions(desc)) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mirror_rtv_candidate_mtx);
    uint32_t count = g_mirror_rtv_candidate_count.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < count; ++i) {
        if (g_mirror_rtv_candidates[i].handle == handle.ptr &&
            g_mirror_rtv_candidates[i].resource == resource) {
            return;
        }
        // Same descriptor slot, different resource: the engine recycled the descriptor, so the
        // old entry is stale and must go rather than shadow the new one on lookup.
        if (g_mirror_rtv_candidates[i].handle == handle.ptr) {
            if (g_mirror_rtv_candidates[i].resource) g_mirror_rtv_candidates[i].resource->Release();
            resource->AddRef();
            g_mirror_rtv_candidates[i] = { handle.ptr, resource, desc.Format, view_format };
            ++CyberpunkVR_DebugMirrorRtvRegs;
            return;
        }
    }
    uint32_t slot;
    if (count < g_mirror_rtv_candidates.size()) {
        slot = count;
        g_mirror_rtv_candidate_count.store(count + 1, std::memory_order_release);
    } else {
        // THE RING MUST NOT BE ABLE TO DROP THE ONE TARGET WE NEED, which is what the comment on
        // the table above has always claimed and what the plain wrapping cursor did not deliver.
        // A wrap threw out the vrcam output's entry, its handle stopped resolving, and the second
        // eye went mono with the node still binding it every frame -- measured, evicts climbing
        // 200 per five seconds against a 512-slot table.
        //
        // So the cursor walks past any slot holding a resource we have published as a vrcam
        // output. At most 64 of those against 512 slots, so a victim is always found; the loop is
        // bounded anyway and falls back to evicting the cursor's own slot rather than spinning.
        const uint32_t n = (uint32_t)g_mirror_rtv_candidates.size();
        slot = g_mirror_rtv_next.fetch_add(1, std::memory_order_relaxed) % n;
        for (uint32_t tries = 0; tries < n; ++tries) {
            if (!mirror_is_pinned_output(g_mirror_rtv_candidates[slot].resource)) break;
            slot = g_mirror_rtv_next.fetch_add(1, std::memory_order_relaxed) % n;
        }
        static bool s_wrapLogged = false;
        if (!s_wrapLogged) {
            s_wrapLogged = true;
            log("[mirror] the RTV candidate table (%u) has wrapped -- evicting from now on, "
                "published vrcam outputs excepted", n);
        }
        if (g_mirror_rtv_candidates[slot].resource) {
            g_mirror_rtv_candidates[slot].resource->Release();
            ++CyberpunkVR_DebugMirrorRtvEvicts;
        }
    }
    resource->AddRef();
    g_mirror_rtv_candidates[slot] = { handle.ptr, resource, desc.Format, view_format };
    ++CyberpunkVR_DebugMirrorRtvRegs;
}

static ID3D12Resource* mirror_find_bound_rtv(SIZE_T handle,
        DXGI_FORMAT* view_format) {
    const uint32_t count =
        g_mirror_rtv_candidate_count.load(std::memory_order_acquire);
    for (uint32_t i = count; i > 0; --i) {
        const MirrorRtvCandidate& candidate = g_mirror_rtv_candidates[i - 1];
        if (candidate.handle == handle) {
            if (view_format) *view_format = candidate.view_format;
            return candidate.resource;
        }
    }
    return nullptr;
}

// Broad RTV->dims (any format/size). Populated from hk_CreateRTV; read by hk_OMSetRenderTargets.
static void rtv_dim_register(D3D12_CPU_DESCRIPTOR_HANDLE handle, ID3D12Resource* res) {
    if (!handle.ptr || !res) return;
    D3D12_RESOURCE_DESC d{};
    if (!mirror_get_resource_desc(res, &d) ||
        d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return;
    const uint32_t cap = (uint32_t)g_rtv_dim_map.size();
    std::lock_guard<std::mutex> lk(g_rtv_dim_mtx);
    uint32_t n = g_rtv_dim_count.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < n; ++i) {
        if (g_rtv_dim_map[i].handle.load(std::memory_order_relaxed) == handle.ptr) {
            // Same descriptor, re-created. Take it out of service while the fields change, so a
            // concurrent lookup cannot pair this handle with the previous resource.
            g_rtv_dim_map[i].handle.store(0, std::memory_order_release);
            g_rtv_dim_map[i].w = (uint32_t)d.Width;
            g_rtv_dim_map[i].h = d.Height;
            g_rtv_dim_map[i].res = res;
            g_rtv_dim_map[i].handle.store(handle.ptr, std::memory_order_release);
            return;
        }
    }
    uint32_t slot;
    if (n < cap) {
        slot = n;
    } else {
        slot = g_rtv_dim_next.fetch_add(1, std::memory_order_relaxed) % cap;
        if (!g_rtv_dim_wrapped_logged) {
            g_rtv_dim_wrapped_logged = true;
            log("[mirror] the RTV descriptor map (%u) has wrapped -- the oldest descriptors stop "
                "resolving from here. HUD identification and vrcam capture both read it.", cap);
        }
    }
    g_rtv_dim_map[slot].handle.store(0, std::memory_order_release);
    g_rtv_dim_map[slot].w = (uint32_t)d.Width;
    g_rtv_dim_map[slot].h = d.Height;
    g_rtv_dim_map[slot].res = res;
    g_rtv_dim_map[slot].handle.store(handle.ptr, std::memory_order_release);
    if (n < cap) g_rtv_dim_count.store(n + 1, std::memory_order_release);
}
static ID3D12Resource* rtv_resource_lookup(SIZE_T handle) {
    const uint32_t n = g_rtv_dim_count.load(std::memory_order_acquire);
    for (uint32_t i = n; i > 0; --i)
        if (g_rtv_dim_map[i - 1].handle.load(std::memory_order_acquire) == handle)
            return g_rtv_dim_map[i - 1].res;
    return nullptr;
}

static bool rtv_dim_lookup(SIZE_T handle, uint32_t* w, uint32_t* h) {
    const uint32_t n = g_rtv_dim_count.load(std::memory_order_acquire);
    for (uint32_t i = n; i > 0; --i) {
        if (g_rtv_dim_map[i - 1].handle.load(std::memory_order_acquire) == handle) {
            *w = g_rtv_dim_map[i - 1].w; *h = g_rtv_dim_map[i - 1].h;
            return true;
        }
    }
    return false;
}

// 64, NOT 8, AND IT SAYS SO WHEN IT FILLS.
//
// This is the AddRef list for the vrcam output targets. Eight was sized for the ring the engine
// rotates in a steady graph -- and a session log reached ring[7] with two more arriving back to
// back at the end, so it was full. Past that the resource is still tracked as the current output
// but never retained, which is a raw pointer into something the engine is free to destroy.
//
// The graph rebuilds are what overruns it: every map or inventory open builds a new graph with new
// dtex targets, so the set grows with the number of menus opened rather than with the ring size.
// Exactly the shape of the upload-map bug fixed earlier -- eight slots, silently full, everything
// after it dropped on the floor with no line in the log to say so. Same fix, and the same
// saturation notice, because the silence was the expensive part.
static ID3D12Resource* g_mirror_seen[64] = {};
static int g_mirror_seen_n = 0;
static bool g_mirror_seen_full_logged = false;
// Update the current vrcam output EVERY frame: the engine writes the vrcam final
// into a small ring of persistent committed dtex targets and rotates them, so
// capturing once => stale/black on the other frames. AddRef each unique resource
// once (they live for the whole session) so the copy path can safely use it.
// WHY THE OUTPUT WENT QUIET, ANSWERED BY THE LOG INSTEAD OF BY GUESSWORK.
//
// The second view keeps rendering after the inventory closes -- measured: view-create still firing
// at hits=4201, the vrcam DrawHUD counter still climbing -- while the headset falls to mono. So
// what stops is the CAPTURE of its output, here, and every way out of this function is silent.
// Three guesses have already been spent on which one it is. These counters and the watchdog below
// cost nothing and end the argument: whichever number is climbing when the eye goes stale names
// the branch.
static uint64_t g_pub_rej_foreign = 0, g_pub_rej_desc = 0, g_pub_rej_dims = 0, g_pub_same = 0;
static uint64_t g_pub_ok = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugPubOk = 0;

static void mirror_publish_output(ID3D12Resource* resource,
        DXGI_FORMAT view_format) {
    // Watchdog: the output has not been refreshed for a second while binds keep arriving. That is
    // the state the headset shows as mono, so say it once with the tally that explains it.
    {
        static uint64_t s_lastOkMs = 0, s_lastWarnMs = 0;
        const uint64_t now = GetTickCount64();
        if (!s_lastOkMs) s_lastOkMs = now;
        if (g_pub_ok != CyberpunkVR_DebugPubOk) { CyberpunkVR_DebugPubOk = g_pub_ok; s_lastOkMs = now; }
        if (now - s_lastOkMs > 1000 && now - s_lastWarnMs > 5000) {
            s_lastWarnMs = now;
            log("[mirror] vrcam output has not been accepted for %llu ms -- the second eye is "
                "going stale. rejected: foreign=%llu desc=%llu dims=%llu, unchanged=%llu, "
                "accepted=%llu",
                (unsigned long long)(now - s_lastOkMs),
                (unsigned long long)g_pub_rej_foreign, (unsigned long long)g_pub_rej_desc,
                (unsigned long long)g_pub_rej_dims, (unsigned long long)g_pub_same,
                (unsigned long long)g_pub_ok);
        }
    }
    if (!resource || mirror_is_foreign(resource)) { ++g_pub_rej_foreign; return; }
    D3D12_RESOURCE_DESC desc{};
    if (!mirror_get_resource_desc(resource, &desc)) { ++g_pub_rej_desc; return; }
    if (!mirror_target_dimensions(desc)) { ++g_pub_rej_dims; return; }
    ++g_pub_ok;
    // Same resource as last time is the NORMAL case on a still frame, not a failure -- counted
    // separately so it cannot be mistaken for one in the tally above.
    if (g_captured_vrcam_res.load(std::memory_order_acquire) == resource) { ++g_pub_same; return; }
    {
        std::lock_guard<std::mutex> lk(g_mirror_resource_mtx);
        bool seen = false;
        for (int i = 0; i < g_mirror_seen_n; ++i)
            if (g_mirror_seen[i] == resource) { seen = true; break; }
        if (!seen && g_mirror_seen_n < static_cast<int>(std::size(g_mirror_seen))) {
            resource->AddRef();
            g_mirror_seen[g_mirror_seen_n++] = resource;
            // Pin it against RTV-table eviction. The list above exists to hold a reference; this
            // publishes the same set lock-free so mirror_register_rtv can refuse to throw the
            // entry away when the table wraps -- the failure that took the second eye.
            const uint32_t pin = g_mirror_pinned_n.load(std::memory_order_relaxed);
            if (pin < g_mirror_pinned.size()) {
                g_mirror_pinned[pin].store(resource, std::memory_order_relaxed);
                g_mirror_pinned_n.store(pin + 1, std::memory_order_release);
            }
            log("[mirror] vrcam output ring[%d]=%p %llux%u fmt=%u rtv=%u",
                g_mirror_seen_n - 1, resource, (unsigned long long)desc.Width,
                desc.Height, (unsigned)desc.Format, (unsigned)view_format);
        } else if (!seen && !g_mirror_seen_full_logged) {
            g_mirror_seen_full_logged = true;
            log("[mirror] vrcam output list FULL at %d -- further targets are used without a "
                "reference. If the second eye starts dropping to mono, this is the first place "
                "to look.", g_mirror_seen_n);
        }
    }
    g_captured_vrcam_res.store(resource, std::memory_order_release);
    CyberpunkVR_DebugMirrorFmt = (uint32_t)desc.Format;
    CyberpunkVR_DebugMirrorRes = reinterpret_cast<uint64_t>(resource);
}

// ---- THE HUD IS ITS OWN TEXTURE, AND IT CAN SIMPLY BE COPIED ---------------------------------
//
// Settled from two independent Nsight captures rather than from theory. The HUD is NOT drawn into
// the scene colour; the composition group renders it into a dedicated full-resolution surface:
//
//     DiscardResource(hudTex, NumSubresources = 5)
//     ClearRenderTargetView(mip0, {0,0,0,0})        <- cleared TRANSPARENT
//     OMSetRenderTargets(1, mip0); RSSetViewports(output res)
//     ... ~29 ink quad draws (stride-24 verts, blend ONE / INV_SRC_ALPHA) ...
//     OMSetRenderTargets(0, nullptr)                <- the snapshot below goes exactly here
//     ... mips 1..4 generated for the HUD glow ...
//
// and RenderFinal2D then composites that surface over the scene in a single fullscreen draw.
//
// Those draws are recorded into the "PostFX" command list, OUTSIDE the DrawHUD node's dynamic
// extent -- which is why the earlier node-scoped OMSetRenderTargets probe saw nothing and I
// concluded, wrongly, that no separable HUD surface existed. It does.
//
// Identification is exact, not heuristic: across all 23209 resources in the capture, exactly ONE
// is an RGBA8 render target with MipLevels != 1. That is the whole signature.
//
// Blend ONE / INV_SRC_ALPHA means the surface holds PREMULTIPLIED alpha, so the second eye wants
//     out.rgb = hud.rgb + eye.rgb * (1 - hud.a)
// which is what ColorBlit::RecordOverlay does.
//
// Nothing here touches engine state: the copy is a barrier pair around a CopyResource appended to
// the engine's own list, the same shape as mirror_stable_inline_copy, which has been stable.
extern "C" __declspec(dllexport) int      CyberpunkVR_HudToSecondEye   = 1;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugHudSnaps    = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugHudSnapSkips = 0;

static bool hud_rt_signature(const D3D12_RESOURCE_DESC& d) {
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return false;
    if (d.MipLevels <= 1 || d.DepthOrArraySize != 1 || d.SampleDesc.Count != 1) return false;
    if (!(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) return false;
    if (d.Width < 640 || d.Height < 360) return false;   // never a small ink widget target
    return d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           d.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
           d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
}

// ONE current HUD surface, held by reference, with the handful of mip-0 RTV handles that point
// at it. Deliberately not a growing handle->resource table: the engine recreates the HUD surface
// (graph rebuild, resolution change, enabling the mirror), and a table that keeps the old entries
// would hand a raw, possibly-freed pointer to the copy below, or -- worse -- treat an unrelated
// bind through a recycled descriptor slot as "the HUD is finished" and assert RENDER_TARGET on a
// resource that is nothing of the kind. Registering a new HUD surface therefore RETIRES the old
// one outright, and the resource is AddRef'd for as long as we can name it.
static std::mutex g_hud_rtv_mtx;
static ID3D12Resource* g_hud_res = nullptr;          // AddRef'd
static std::array<SIZE_T, 8> g_hud_handles{};
static std::atomic<uint32_t> g_hud_handle_count{0};
// Subresource index of the HUD's last mip -- the snapshot waits for it, because the composite's
// glow samples mips 1, 2 and 4.
static std::atomic<uint32_t> g_hud_last_mip{0};
// One-shot: dump the contents of the barrier batch that retires the HUD, so the two
// textures we need are identified from what is actually there rather than guessed.
static bool g_hud_batch_listed = false;

// The descriptor signature above is NOT unique, and a log taken in the inventory proves it:
//
//     [hud] surface res=...  1024x1024 mips=11 fmt=27
//
// That is the character-portrait render target -- 1024x1024, a full mip chain, RGBA8 -- and it
// satisfies every clause. It was then adopted as "the HUD" and composited into the second eye,
// which is the yellow full-screen face the menu showed. The signature was validated against one
// gameplay capture ("exactly one such resource in the capture"); menus simply have another one.
//
// So name it the way the outline and the sight were named: by WHO DRAWS INTO IT. rva 0x1EE760 is
// CRenderNode_DrawHUD, the node the RenderMask/HUD descriptor gates, and the render target bound
// while it is on the stack is the HUD surface by construction. Once the node has spoken, the
// descriptor path stops adopting entirely, so no menu resource can take the surface over.
//
// If that node never binds one -- a graph we have not seen -- nothing is named, the flag stays
// false and today's behaviour continues unchanged. This cannot be worse than what it replaces.
// MEASURED, after 0x1EE760 turned out to bind nothing at all. The [hudnode] table, live:
//
//   20A264 : 2560x2560 m5 f29  (M675/V0)     CRenderNode_DrawComposition
//   1F8928 : 2560x2560 m5 f29  (M2696/V0)    CRenderNode_CompositionPostProcess
//
// Those two, and only those two, ever bind a HUD-shaped target -- the same surface each time,
// 2560x2560 with 5 mips in sRGB, exactly the one the capture shows the 2D HUD geometry drawn
// into. 0x1EE760 is where the RenderMask/HUD capability is TESTED, which is not the node body
// that binds; mistaking one for the other is what made the first attempt a silent no-op.
//
// Note what is ABSENT from that table: the inventory's portrait. It is created as an RTV -- which
// is why the descriptor test saw it -- but never bound through this path, so keying on the node
// excludes it by construction rather than by another shape rule.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_HudNodeRva  = 0x1F8928;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_HudNodeRva2 = 0x20A264;
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_HudByNode = 1;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugHudNodeNames = 0;
static std::atomic<bool> g_hud_node_named{false};

extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugHudRearms = 0;

// OFF. It was a bad trade and the log says so.
//
// Clearing g_hud_node_named turns descriptor matching back on, and that test cannot tell the HUD
// from any other mip-chained RGBA8 render target -- the inventory's character portrait among them,
// as the comment beside the hold has always said. Measured after one inventory close: four
// different surfaces adopted twenty-odd times each, the composite flipping in and out nineteen
// times, and the headset dropping to mono about as long after as CyberpunkVR_HudHoldMs runs.
//
// Before the re-arm the composite died once after a menu and stayed dead: the second eye lost its
// HUD, which is bad. After it, the picture strobes and then loses the second eye entirely, which
// is worse. Stable-and-wrong beats unstable-and-wrong until the actual cause is found, so this
// defaults off and stays switchable for the next attempt at it.
extern "C" __declspec(dllexport) int CyberpunkVR_HudRearmOnGraphChange = 0;

// Called from fg_observe when a FULL frame-graph build shows up under a key never seen before --
// a map or inventory open, and the return from one. Everything the HUD path latched belongs to
// the graph that just went away, so let it be found again in the new one:
//
//   node-named    cleared, which re-enables descriptor matching. This is the one that mattered:
//                 once set it turns matching off, so if the rebuilt graph never re-names the node
//                 there is no second way in and the second eye loses the HUD for the session.
//   held surface  released, so a fresh HUD-shaped target is adopted immediately instead of
//                 waiting out CyberpunkVR_HudHoldMs against a surface the engine already dropped.
//   scan tick     zeroed, so the composite constants are looked for on the next frame rather than
//                 up to two seconds later.
//
// Deliberately NOT cleared: g_hud_cb_from_ring and our own constants copy. The copy is still
// valid memory and still ours; the re-scan refreshes what is in it.
static void hud_rearm_for_new_graph(uint64_t key) {
    if (!CyberpunkVR_HudRearmOnGraphChange) return;
    const bool wasNamed = g_hud_node_named.exchange(false, std::memory_order_acq_rel);
    ID3D12Resource* dropped = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_hud_rtv_mtx);
        dropped = g_hud_res;
        g_hud_res = nullptr;
        g_hud_handle_count.store(0, std::memory_order_release);
    }
    if (dropped) dropped->Release();
    g_hud_cb_scan_tick = 0;

    // GIVE THE NEXT SURFACE A GRACE PERIOD, or the re-arm trades one failure for a worse one.
    //
    // Dropping the incumbent lets a descriptor match adopt again immediately -- and then adopt
    // AGAIN the next frame, because the engine pools these targets: 182 distinct HUD surface
    // pointers in one session. The hold that normally prevents that only bites while the incumbent
    // is demonstrably alive, `GetTickCount64() - g_hud_snap_tick < HudHoldMs`, and a surface
    // adopted one frame ago has not produced a snapshot yet. So the tick was stale, the hold
    // lapsed, the newcomer won, repeat. Measured: sixteen complete/waiting flips in eighty log
    // lines after a map close, which is the second eye's HUD strobing for about a second.
    //
    // Stamping the tick here starts the clock at the re-arm instead of at the first snapshot, so
    // whichever surface is adopted first holds the floor long enough to prove itself. Still
    // self-correcting: if it turns out to be the wrong one it produces nothing, the hold lapses on
    // schedule and the next candidate gets its turn.
    g_hud_snap_tick.store(GetTickCount64(), std::memory_order_release);

    ++CyberpunkVR_DebugHudRearms;
    log("[hud] frame graph rebuilt under key %016llX -- identification re-armed "
        "(was %s, surface %s)", (unsigned long long)key,
        wasNamed ? "node-named" : "unnamed", dropped ? "dropped" : "none held");
}

// The node named in the render-mask table (0x1EE760) never bound a target through this hook --
// zero [hud] surface named by DrawHUD lines in a whole session -- so that rva is the function
// that TESTS the HUD capability, not the node body that binds. Rather than try another guess,
// record which node binds each HUD-shaped target and read the answer off the log.
extern "C" __declspec(dllexport) int32_t CyberpunkVR_HudNodeProbe = 1;   // answered; the two
// binding nodes are hardcoded above. Set to 1 to re-run the survey after an engine update.
struct HudNodeCand { uint32_t rva, w, h, mips, fmt; uint64_t hits[2]; };
static std::array<HudNodeCand, 24> g_hudnode{};
static uint32_t g_hudnode_n = 0;
static std::mutex g_hudnode_mtx;

static void hud_node_note(ID3D12Resource* res, bool vrcam) {
    D3D12_RESOURCE_DESC d{};
    if (!mirror_get_resource_desc(res, &d) || !hud_rt_signature(d)) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(g_exe_base);
    const uintptr_t work = t_current_node_work;
    const uint32_t rva = (base && work > base) ? static_cast<uint32_t>(work - base) : 0;
    {
        std::lock_guard<std::mutex> lk(g_hudnode_mtx);
        uint32_t i = 0;
        for (; i < g_hudnode_n; ++i)
            if (g_hudnode[i].rva == rva && g_hudnode[i].w == (uint32_t)d.Width &&
                g_hudnode[i].h == d.Height && g_hudnode[i].mips == d.MipLevels) break;
        if (i == g_hudnode_n) {
            if (g_hudnode_n >= g_hudnode.size()) return;
            g_hudnode[g_hudnode_n++] = { rva, (uint32_t)d.Width, d.Height,
                                         d.MipLevels, (uint32_t)d.Format, {0, 0} };
        }
        ++g_hudnode[i].hits[vrcam ? 1 : 0];
    }
    static uint64_t s_last = 0;
    const uint64_t now = GetTickCount64();
    if (s_last && now - s_last < 15000) return;
    s_last = now;
    HudNodeCand c[24]; uint32_t n;
    {
        std::lock_guard<std::mutex> lk(g_hudnode_mtx);
        n = g_hudnode_n;
        for (uint32_t i = 0; i < n; ++i) c[i] = g_hudnode[i];
    }
    char line[900]; int u = 0; line[0] = 0;
    for (uint32_t i = 0; i < n && u < static_cast<int>(sizeof(line)) - 60; ++i)
        u += snprintf(line + u, sizeof(line) - u, "%X:%ux%u m%u f%u(M%llu/V%llu) ",
                      c[i].rva, c[i].w, c[i].h, c[i].mips, c[i].fmt,
                      (unsigned long long)c[i].hits[0], (unsigned long long)c[i].hits[1]);
    log("[hudnode] who binds a HUD-shaped target (%u): %s", n, line);
}

static void hud_adopt_by_node(ID3D12Resource* res) {
    if (!res) return;
    D3D12_RESOURCE_DESC d{};
    if (!mirror_get_resource_desc(res, &d)) return;
    // Node AND shape, not one or the other: the node also binds targets that are not the HUD.
    if (!hud_rt_signature(d)) return;
    std::lock_guard<std::mutex> lk(g_hud_rtv_mtx);
    if (res != g_hud_res) {
        g_hud_handle_count.store(0, std::memory_order_release);
        if (g_hud_res) g_hud_res->Release();
        res->AddRef();
        g_hud_res = res;
        g_hud_last_mip.store(d.MipLevels ? d.MipLevels - 1u : 0u, std::memory_order_release);
        // Throttled, because the engine POOLS these: adoption is not an event, it is a heartbeat.
        // 1877 lines of it in one session -- 42% of the whole log -- and every line said the same
        // thing about a different pointer. The count of swallowed ones rides along, so a genuine
        // storm still shows as a storm.
        LOG_THROTTLED_LC(5000,
            "[hud] surface named by DrawHUD: res=%p %llux%u mips=%u fmt=%u (+%llu more adoptions "
            "since the last line -- the engine pools these)", res,
            (unsigned long long)d.Width, d.Height, (unsigned)d.MipLevels, (unsigned)d.Format,
            (unsigned long long)LOG_SKIPPED);
    }
    if (!g_hud_node_named.exchange(true, std::memory_order_acq_rel))
        log("[hud] identification switched to node 0x%X -- descriptor matching disabled",
            CyberpunkVR_HudNodeRva);
    ++CyberpunkVR_DebugHudNodeNames;
}

// How long a producing surface is protected from being replaced by a descriptor match. The
// descriptor test cannot tell the HUD from the inventory's character portrait -- both are
// mip-chained RGBA8 render targets -- but LIVENESS can: the real HUD is the one that keeps
// yielding snapshots. A newcomer only takes over once the incumbent has gone quiet, which is
// exactly what a genuine graph rebuild or resolution change looks like. Self-correcting too: if
// the wrong surface ever gets in, it produces nothing, the hold lapses and the right one wins.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_HudHoldMs = 1500;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugHudHolds = 0;

static void hud_register_rtv(ID3D12Resource* res,
        const D3D12_RENDER_TARGET_VIEW_DESC* vd, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    if (!res || !h.ptr) return;
    // The node has named it: never let a descriptor match override that.
    if (CyberpunkVR_HudByNode && g_hud_node_named.load(std::memory_order_acquire)) return;
    if (CyberpunkVR_HudHoldMs && res != g_hud_res && g_hud_res) {
        const uint64_t t = g_hud_snap_tick.load(std::memory_order_acquire);
        if (t && GetTickCount64() - t < CyberpunkVR_HudHoldMs) {
            ++CyberpunkVR_DebugHudHolds;
            return;
        }
    }
    // Mip 0 ONLY. The engine also creates RTVs for mips 1..4 to build the HUD glow chain; if
    // those counted as "the HUD target" the snapshot would fire after mip generation, when the
    // subresources are no longer uniformly in RENDER_TARGET and the barrier below would lie.
    if (vd && (vd->ViewDimension != D3D12_RTV_DIMENSION_TEXTURE2D ||
               vd->Texture2D.MipSlice != 0)) {
        return;
    }
    D3D12_RESOURCE_DESC d{};
    const bool is_hud = mirror_get_resource_desc(res, &d) && hud_rt_signature(d);

    std::lock_guard<std::mutex> lk(g_hud_rtv_mtx);
    if (!is_hud) {
        // A descriptor slot we thought was the HUD's, reused for something else -> forget it,
        // rather than let a stale handle trigger a copy from the wrong resource.
        const uint32_t n = g_hud_handle_count.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < n; ++i) {
            if (g_hud_handles[i] == h.ptr) {
                g_hud_handles[i] = g_hud_handles[n - 1];
                g_hud_handle_count.store(n - 1, std::memory_order_release);
                break;
            }
        }
        return;
    }
    if (res != g_hud_res) {
        g_hud_handle_count.store(0, std::memory_order_release);   // retire the old surface first
        if (g_hud_res) g_hud_res->Release();
        res->AddRef();
        g_hud_res = res;
        g_hud_last_mip.store(d.MipLevels ? d.MipLevels - 1u : 0u, std::memory_order_release);
        log("[hud] surface res=%p %llux%u mips=%u fmt=%u", res,
            (unsigned long long)d.Width, d.Height, (unsigned)d.MipLevels, (unsigned)d.Format);
    }
    uint32_t n = g_hud_handle_count.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < n; ++i) if (g_hud_handles[i] == h.ptr) return;
    if (n >= g_hud_handles.size()) return;
    g_hud_handles[n] = h.ptr;
    g_hud_handle_count.store(n + 1, std::memory_order_release);
}


// The engine's own blurred-HUD pyramid (half resolution, 4 mips). The composite adds it at lod
// 1.8 with weight _43_m0[5].x -- that is the wide halo on the map, the weapon icons and the
// tracked quest, and it also feeds the shadow term. It is HUD, not scene bloom: the shadow lerps
// ITS alpha against the HUD's, and scene colour has no meaningful alpha. Reproducing it with our
// own mip chain was close but not equal, so we take the engine's.
static bool hud_blur_signature(const D3D12_RESOURCE_DESC& d, uint64_t hudWidth) {
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return false;
    if (d.MipLevels != 4 || d.DepthOrArraySize != 1 || d.SampleDesc.Count != 1) return false;
    if (!(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) return false;
    if (d.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS &&
        d.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) return false;
    if (!hudWidth) return false;
    const uint64_t half = hudWidth / 2;
    return d.Width + 1 >= half && d.Width <= half + 1;
}

static std::mutex g_hud_snap_mtx;
// A frame that takes longer than this -- a load spike, a heavy menu -- would blink the HUD off,
// so the window is generous: it only has to be shorter than a menu, not shorter than a hitch.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_HudMaxAgeMs = 1000;
static ID3D12Resource* g_hud_snap = nullptr;
static D3D12_RESOURCE_DESC g_hud_snap_desc{};
static std::atomic<bool> g_hud_snap_fresh{false};
static ID3D12Resource* g_hud_blur_snap = nullptr;
static D3D12_RESOURCE_DESC g_hud_blur_desc{};
static std::atomic<bool> g_hud_blur_fresh{false};
// MAIN's FINISHED frame and the scene that went under it. With both, the second eye needs no
// reproduction of the HUD at all: the engine's composite is
//     out_main = A + S_main * K          (A = every HUD term, K = how it dims the scene)
// so swapping the scene underneath is exact --
//     out_vrcam = out_main + (S_vrcam - S_main) * K
// and the HUD pixels are literally the engine's, curvature, glow, halo, flicker and all.
// out_main is snapshotted as TYPELESS so it can be read through an _UNORM_SRGB view: the engine
// encodes to sRGB inside the composite, while the scenes are linear.
static ID3D12Resource* g_main_out_snap = nullptr;
static D3D12_RESOURCE_DESC g_main_out_desc{};
static std::atomic<bool> g_main_out_fresh{false};
static ID3D12Resource* g_main_scene_snap = nullptr;
static D3D12_RESOURCE_DESC g_main_scene_desc{};
static std::atomic<bool> g_main_scene_fresh{false};
// The scanner's object outline, as the SECOND eye draws it. Not copied from MAIN: the outline
// traces on-screen silhouettes, so MAIN's would sit at MAIN's parallax and read as double
// vision. VRCAM renders its own -- Resource_83328 at 2444x2560 in EventList_SCANERELEMENTS,
// against MAIN's Resource_85164 -- it is simply never composited, because the chain that turns
// it into a visible outline (PS587 -> PS1047 -> PS1290 -> the PS1216 composite) is MAIN-only,
// behind the same viewData+0x168 gate as the HUD.
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_VisionSnap       = 1;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugVisionSnaps = 0;
static ID3D12Resource* g_vision_snap = nullptr;
static D3D12_RESOURCE_DESC g_vision_desc{};
static std::atomic<bool> g_vision_fresh{false};
// When the node stops running (view torn down, scanner gone) the last snapshot would otherwise
// keep painting an outline over the second eye for ever. Time-stamped, so it simply expires.
static std::atomic<uint64_t> g_vision_tick{0};
// Demand-driven: the copy is ~35 MB a frame at 2560x2560x5mips, so it must not run when nothing
// consumes it. Measured with the headset off, it ran every single frame for nothing. The eye-1
// path stamps this each time it takes the texture; one bootstrap copy is allowed so the very
// first consumer has something to ask for, and after that the copy follows demand.
static std::atomic<uint64_t> g_hud_consumed_tick{0};
// Which list has the HUD target bound, on this recording thread. Both are needed: the resource
// says WHAT to copy, the list says the pending bind still belongs to the list we are in, so a
// list abandoned mid-HUD can never make us barrier a resource that is no longer a render target.
static thread_local ID3D12Resource* t_hud_rt_bound = nullptr;
static thread_local ID3D12GraphicsCommandList* t_hud_rt_list = nullptr;

// Append, to the engine's OWN list, a copy of the finished HUD surface into a committed texture
// of ours. Recorded at the unbind, so in queue order it lands after the last HUD draw and before
// the mip chain -- and before any aliasing barrier can recycle the transient's heap memory, which
// is the whole reason this cannot be done later from the Present thread.
enum HudSnapSlot { kSnapHud = 0, kSnapBlur, kSnapMainOut, kSnapMainScene, kSnapVision };

static void hud_snapshot_copy(ID3D12GraphicsCommandList* list, ID3D12Resource* src,
                              int which, D3D12_RESOURCE_STATES rest) {
    if (!g_game_device || !list || !src) return;
    const CommandListVtableHook* e = command_list_hook_entry(list);
    D3D12_RESOURCE_DESC d{};
    if (!e || !e->barrier_call || !e->copyres || !mirror_get_resource_desc(src, &d)) {
        InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugHudSnapSkips));
        return;
    }
    ID3D12Resource** slots[5] = { &g_hud_snap, &g_hud_blur_snap,
                                  &g_main_out_snap, &g_main_scene_snap, &g_vision_snap };
    D3D12_RESOURCE_DESC* descs[5] = { &g_hud_snap_desc, &g_hud_blur_desc,
                                      &g_main_out_desc, &g_main_scene_desc, &g_vision_desc };
    std::atomic<bool>* freshes[5] = { &g_hud_snap_fresh, &g_hud_blur_fresh,
                                      &g_main_out_fresh, &g_main_scene_fresh, &g_vision_fresh };
    static const wchar_t* names[5] = { L"CyberpunkVR_HudSnapshot", L"CyberpunkVR_HudBlurSnapshot",
                                       L"CyberpunkVR_MainOutSnapshot",
                                       L"CyberpunkVR_MainSceneSnapshot",
                                       L"CyberpunkVR_VisionSnapshot" };
    if (which < 0 || which > 4) return;
    ID3D12Resource*& slot = *slots[which];
    D3D12_RESOURCE_DESC& slotDesc = *descs[which];
    std::atomic<bool>& slotFresh = *freshes[which];
    ID3D12Resource* snap = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_hud_snap_mtx);
        // Resolution or format change: intentionally leak the old snapshot rather than free a
        // texture a copy recorded this frame may still reference (rare, and it is 8 MB).
        //
        // "Rare" is load-bearing, and it stopped being true once a slot was fed by a detector
        // that matched two different sizes: the slot re-allocated on alternate calls and leaked
        // 1527 committed textures (~24 GB) in a single session. A caller that alternates is a
        // BUG in the caller, so cap the churn here and say so, rather than let it run away.
        if (slot && (slotDesc.Width != d.Width ||
                     slotDesc.Height != d.Height ||
                     slotDesc.Format != d.Format ||
                     slotDesc.MipLevels != d.MipLevels)) {
            static uint32_t s_realloc[5] = {0, 0, 0, 0, 0};
            if (++s_realloc[which] > 8) {
                if (s_realloc[which] == 9)
                    log("[hud] snapshot[%d] REFUSED: caller alternates between descs "
                        "(%llux%u fmt=%u vs %llux%u fmt=%u) -- ignoring further changes",
                        which, (unsigned long long)slotDesc.Width, slotDesc.Height,
                        (unsigned)slotDesc.Format, (unsigned long long)d.Width, d.Height,
                        (unsigned)d.Format);
                InterlockedIncrement64(
                    reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugHudSnapSkips));
                return;
            }
            slot = nullptr;
            slotFresh.store(false, std::memory_order_release);
        }
        if (!slot) {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            hp.CreationNodeMask = hp.VisibleNodeMask = 1;
            D3D12_RESOURCE_DESC nd = d;
            nd.Flags = D3D12_RESOURCE_FLAG_NONE;      // plain copy target, sampled by us only
            // MAIN's finished frame holds sRGB-encoded bytes; typeless lets us decode on read.
            if (which == kSnapMainOut && nd.Format == DXGI_FORMAT_R8G8B8A8_UNORM)
                nd.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
            ID3D12Resource* tex = nullptr;
            if (FAILED(g_game_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &nd,
                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&tex))) || !tex) {
                InterlockedIncrement64(
                    reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugHudSnapSkips));
                return;
            }
            tex->SetName(names[which]);
            slot = tex;
            slotDesc = d;
            log("[hud] snapshot[%d]=%p %llux%u mips=%u fmt=%u", which, tex,
                (unsigned long long)d.Width, d.Height, (unsigned)d.MipLevels, (unsigned)d.Format);
        }
        snap = slot;
    }
    // The caller names the state the source is actually resting in; every snapshot here is taken
    // at a barrier where that is known exactly, so the transition below is never a guess.
    const D3D12_RESOURCE_STATES kHudRest = rest;
    D3D12_RESOURCE_BARRIER b[2]{};
    for (int i = 0; i < 2; ++i) {
        b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    b[0].Transition.pResource = src;
    b[0].Transition.StateBefore = kHudRest;
    b[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Transition.pResource = snap;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    e->barrier_call(list, 2, b);
    e->copyres(list, snap, src);
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[0].Transition.StateAfter  = kHudRest;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    e->barrier_call(list, 2, b);
    slotFresh.store(true, std::memory_order_release);
    if (which == kSnapHud) g_hud_snap_tick.store(GetTickCount64(), std::memory_order_release);
    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugHudSnaps));
}

// ---- the engine's HUD composite constants ----------------------------------------------------
// Read out of the game's own b6 buffer at the composite dispatch (PipelineState_576), so these
// are the engine's live values, not a fit by eye. Exported so they can be tuned without a
// rebuild if a graphics setting turns out to move them.
extern "C" __declspec(dllexport) float CyberpunkVR_HudCurvatureX   = 0.009017f;
extern "C" __declspec(dllexport) float CyberpunkVR_HudCurvatureY   = 0.084242f;
extern "C" __declspec(dllexport) float CyberpunkVR_HudAberration   = 0.0001f;
extern "C" __declspec(dllexport) float CyberpunkVR_HudGain         = 2.0f;    // the engine's x2
extern "C" __declspec(dllexport) float CyberpunkVR_HudGlowGain     = 1.0f;
extern "C" __declspec(dllexport) float CyberpunkVR_HudShadow       = 1.0f;
// The wide halo (map, weapon icons, tracked quest). Gain is the engine's _43_m0[5].x; the lod is
// its 1.8 rebased from the half-res pyramid it samples onto our full-res mip chain.
extern "C" __declspec(dllexport) float CyberpunkVR_HudBloomGain    = 0.65f;
extern "C" __declspec(dllexport) float CyberpunkVR_HudBloomLod     = 1.8f;
// Sharpness bisection: 0 = full composite, 1 = HUD term only, 2 = no curvature warp,
// 3 = no halo, 4 = no glow, 5 = no aberration. Live-switchable, no rebuild.
extern "C" __declspec(dllexport) float CyberpunkVR_HudDebugMode    = 0.0f;
// On, and on the engine's own frame time (bound as b1 from its own constant buffer). This is not
// cosmetic: the flicker term gates the glow, and its mean is about 0.66 -- forcing it to 1 made
// the halo half again too strong and visibly softened the text.
extern "C" __declspec(dllexport) float CyberpunkVR_HudFlicker      = 1.0f;

static ColorBlit::HudParams hud_composite_params() {
    ColorBlit::HudParams p{};              // defaults are the captured values
    p.curvature[0]   = CyberpunkVR_HudCurvatureX;
    p.curvature[1]   = CyberpunkVR_HudCurvatureY;
    p.aberration     = CyberpunkVR_HudAberration;
    p.hudGain        = CyberpunkVR_HudGain;
    p.glowGain       = CyberpunkVR_HudGlowGain;
    p.shadowStrength = CyberpunkVR_HudShadow;
    p.bloomGain      = CyberpunkVR_HudBloomGain;
    p.bloomLod       = CyberpunkVR_HudBloomLod;
    p.flicker        = CyberpunkVR_HudFlicker;
    p.debugMode      = CyberpunkVR_HudDebugMode;
    (void)0;
    p.time           = (float)(GetTickCount64() % 100000ull) * 0.001f;
    return p;
}

// The finished HUD, premultiplied-alpha, in the game's output resolution. Null until the first
// frame that actually drew a HUD (loading screens, photo mode with HUD off, and so on).
// Taking it counts as demand -- see g_hud_consumed_tick.
// The second eye's own outline layer, premultiplied like the HUD surface. Null until a frame
// has actually drawn one (i.e. until something is being scanned).
extern "C" __declspec(dllexport) ID3D12Resource* CyberpunkVR_GetVisionTexture() {
    if (!CyberpunkVR_VisionSnap) return nullptr;
    if (!g_vision_fresh.load(std::memory_order_acquire)) return nullptr;
    const uint64_t t = g_vision_tick.load(std::memory_order_acquire);
    if (!t || GetTickCount64() - t > 250) return nullptr;   // stale -> no overlay
    std::lock_guard<std::mutex> lk(g_hud_snap_mtx);
    return g_vision_snap;
}

extern "C" __declspec(dllexport) ID3D12Resource* CyberpunkVR_GetHudTexture() {
    if (!CyberpunkVR_HudToSecondEye) return nullptr;
    // DEMAND IS RECORDED FIRST, AND UNCONDITIONALLY. The producer gate in hk_ResourceBarrier
    // keys on this tick -- it only takes a snapshot while someone is asking for one -- so it
    // must mean "the consumer asked", never "the consumer got something". Deriving it from the
    // return value instead makes the two latch each other off: one stale frame stops demand,
    // two seconds later production stops, and the snapshot can never become fresh again. That
    // is exactly what killed the HUD when the age check below was first added.
    g_hud_consumed_tick.store(GetTickCount64(), std::memory_order_release);
    if (!g_hud_snap_fresh.load(std::memory_order_acquire)) return nullptr;
    // Liveness, not existence. The fresh flag latches on the first copy and never clears, so when
    // the engine stops drawing a HUD -- a menu, a load -- the second eye would keep compositing
    // the last one it saw. 0 disables the limit.
    if (CyberpunkVR_HudMaxAgeMs) {
        const uint64_t t = g_hud_snap_tick.load(std::memory_order_acquire);
        if (!t || GetTickCount64() - t > CyberpunkVR_HudMaxAgeMs) return nullptr;
    }
    std::lock_guard<std::mutex> lk(g_hud_snap_mtx);
    return g_hud_snap;
}

// The engine's blurred-HUD pyramid, and the frame exposure the composite scales everything by
// (_1472). Both are the engine's own data, so the result is its arithmetic, not an imitation.
extern "C" __declspec(dllexport) ID3D12Resource* CyberpunkVR_GetHudBlurTexture() {
    if (!CyberpunkVR_HudToSecondEye) return nullptr;
    if (!g_hud_blur_fresh.load(std::memory_order_acquire)) return nullptr;
    std::lock_guard<std::mutex> lk(g_hud_snap_mtx);
    return g_hud_blur_snap;
}
extern "C" __declspec(dllexport) ID3D12Resource* CyberpunkVR_GetMainOutTexture() {
    if (!CyberpunkVR_HudToSecondEye) return nullptr;
    if (!g_main_out_fresh.load(std::memory_order_acquire)) return nullptr;
    std::lock_guard<std::mutex> lk(g_hud_snap_mtx);
    return g_main_out_snap;
}
extern "C" __declspec(dllexport) ID3D12Resource* CyberpunkVR_GetMainSceneTexture() {
    if (!CyberpunkVR_HudToSecondEye) return nullptr;
    if (!g_main_scene_fresh.load(std::memory_order_acquire)) return nullptr;
    std::lock_guard<std::mutex> lk(g_hud_snap_mtx);
    return g_main_scene_snap;
}
extern "C" __declspec(dllexport) ID3D12Resource* CyberpunkVR_GetHudExposureBuffer() {
    // MAIN's accumulator: the composite we are reproducing is MAIN's, so the HUD must be scaled
    // by the same exposure in both eyes or they would not match. Rests shader-readable.
    return g_expo_main.load(std::memory_order_acquire);
}
extern "C" __declspec(dllexport) ID3D12Resource* CyberpunkVR_GetFrameConstantBuffer() {
    return g_frame_cb.load(std::memory_order_acquire);
}
// Sweep the mapped upload rings for the composite's constants and take a private copy, so what
// gets bound is ours and cannot be recycled under us by the engine's ring allocator.
static void hud_cb_rescan() {
    if (!g_game_device) return;
    const uint64_t now = GetTickCount64();
    if (g_hud_cb_scan_tick && now - g_hud_cb_scan_tick < 2000) return;
    g_hud_cb_scan_tick = now;

    float w = 0.0f, h = 0.0f;
    {
        std::lock_guard<std::mutex> lk(g_hud_snap_mtx);
        w = static_cast<float>(g_hud_snap_desc.Width);
        h = static_cast<float>(g_hud_snap_desc.Height);
    }
    if (!(w > 0.0f && h > 0.0f)) return;

    // Snapshot the map list and let the lock go: the sweep takes milliseconds and the render
    // thread registers uploads through the same lock.
    MappedUpload local[64]{};
    uint32_t localN = 0;
    {
        std::lock_guard<std::mutex> lk(g_upload_map_mtx);
        localN = g_upload_map_n;
        for (uint32_t i = 0; i < localN; ++i) local[i] = g_upload_maps[i];
    }
    // Stay on the block already in use for as long as it still qualifies. Searching afresh every
    // time is what made the look flip between two sets of values: the ring holds several blocks
    // that satisfy the fingerprint (previous frames' copies among them), and whichever came first
    // in the sweep won. Re-search only once the held block stops being valid.
    static const uint8_t* s_held = nullptr;
    const uint8_t* found = (s_held && hud_cb_block_plausible(s_held, w, h)) ? s_held : nullptr;
    for (uint32_t i = 0; i < localN && !found; ++i) {
        const MappedUpload& m = local[i];
        if (!m.ptr || m.size < 512) continue;
        for (uint64_t off = 0; off + 512 <= m.size; off += 256) {
            if (hud_cb_block_plausible(m.ptr + off, w, h)) { found = m.ptr + off; break; }
        }
    }
    // The ring genuinely moves the block every few frames, so re-finding it is normal and not
    // worth a log line. It was only ever a problem while the fingerprint could match a block of
    // zeros; with that rejected, every block it finds carries the same settings.
    s_held = found;
    if (!found) return;

    if (!g_hud_cb_copy) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        hp.CreationNodeMask = hp.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 512;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* buf = nullptr;
        if (FAILED(g_game_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf))) || !buf) {
            return;
        }
        void* mapped = nullptr;
        D3D12_RANGE none{0, 0};
        if (FAILED(buf->Map(0, &none, &mapped)) || !mapped) { buf->Release(); return; }
        buf->SetName(L"CyberpunkVR_HudConstants");
        g_hud_cb_copy = buf;
        g_hud_cb_copy_ptr = static_cast<uint8_t*>(mapped);
    }
    memcpy(g_hud_cb_copy_ptr, found, 512);
    // Take over even from an earlier capture: this buffer is ours, so it cannot be recycled by the
    // ring allocator under the shader, and the block was found by the full fingerprint. Only the
    // first hand-over logs; after that the copy above just refreshes the contents in place.
    if (!g_hud_cb_from_ring.load(std::memory_order_acquire)) {
        ID3D12Resource* prev = g_hud_cb.exchange(g_hud_cb_copy, std::memory_order_acq_rel);
        g_hud_cb_from_ring.store(true, std::memory_order_release);
        if (prev && prev != g_hud_cb_copy) prev->Release();
        CyberpunkVR_DebugHudCb = reinterpret_cast<uint64_t>(g_hud_cb_copy);
        const float* r = reinterpret_cast<const float*>(g_hud_cb_copy_ptr);
        log("[hud] composite constants found in the upload ring%s: target=%.0fx%.0f "
            "curvature=(%.6f, %.6f) glow=(%.3f, %.3f, %.3f) aberration=%.6f halo=(%.3f lod %.2f)",
            prev ? " (replacing the copy-path capture)" : "",
            r[16 * 4 + 2], r[16 * 4 + 3], r[3 * 4 + 0], r[3 * 4 + 1],
            r[8 * 4 + 2], r[8 * 4 + 3], r[9 * 4 + 0], r[6 * 4 + 3],
            r[5 * 4 + 0], r[5 * 4 + 1]);
    }
}

// WHICH INPUT IS THE HUD STILL WAITING FOR? The composite needs five, and when any is missing it
// falls back to a plain blit -- silently, which is why "the HUD takes a while to come up" was only
// ever a guess about which one was late. Logged once per changed combination, so a normal session
// prints two lines: what it waited for, and that it stopped waiting.
extern "C" __declspec(dllexport) void CyberpunkVR_NoteHudCompositeInputs(
        const void* hud, const void* blur, const void* expo,
        const void* frameCb, const void* hudCb) {
    const uint32_t mask = (hud ? 1u : 0u) | (blur ? 2u : 0u) | (expo ? 4u : 0u) |
                          (frameCb ? 8u : 0u) | (hudCb ? 16u : 0u);
    static std::atomic<uint32_t> s_last{0xFFFFFFFFu};
    if (s_last.exchange(mask, std::memory_order_acq_rel) == mask) return;
    if (mask == 31u) {
        log("[hud] composite inputs complete -- the second eye gets the HUD");
        return;
    }
    // WHEN THE SURFACE IS THE MISSING INPUT, SAY WHY IN THE SAME BREATH.
    //
    // "waiting on: surface" has appeared after every menu for weeks and never carried the one fact
    // that separates the possibilities. The HUD node keeps running -- its DrawHUD counter climbs --
    // so either it binds nothing shaped like the HUD, or it binds it and we cannot say what the
    // descriptor points at. The second is measurable and, going by the RTV map having silently
    // stopped accepting new descriptors at 2048, is the likely one: `bound` comes back null and
    // hud_adopt_by_node is simply never called.
    //
    // nodeBinds vs unresolved answers it outright, and the map fill says whether the map is why.
    if (!(mask & 1u)) {
        log("[hud] composite waiting on: surface%s%s%s%s | hud node binds=%llu of which"
            " unresolved=%llu | rtv map %u/%u%s",
            (mask & 2u) ? "" : " blur-pyramid", (mask & 4u) ? "" : " exposure",
            (mask & 8u) ? "" : " frame-constants", (mask & 16u) ? "" : " composite-constants",
            (unsigned long long)g_hud_node_binds.load(std::memory_order_relaxed),
            (unsigned long long)g_hud_node_unresolved.load(std::memory_order_relaxed),
            g_rtv_dim_count.load(std::memory_order_relaxed),
            (unsigned)g_rtv_dim_map.size(),
            g_rtv_dim_wrapped_logged ? " (wrapped)" : "");
        return;
    }
    log("[hud] composite waiting on:%s%s%s%s%s",
        (mask & 1u)  ? "" : " surface",
        (mask & 2u)  ? "" : " blur-pyramid",
        (mask & 4u)  ? "" : " exposure",
        (mask & 8u)  ? "" : " frame-constants",
        (mask & 16u) ? "" : " composite-constants");
}

extern "C" __declspec(dllexport) ID3D12Resource* CyberpunkVR_GetHudConstantBuffer() {
    hud_cb_rescan();
    return g_hud_cb.load(std::memory_order_acquire);
}

// ---- what constants does each view's lighting actually get? --------------------------------
// Everything else is now equal by measurement: the same nodes dispatch, the same light array,
// the same bindings. The one asymmetry left standing is the 256-byte block bound at b6, where
// the capture showed MAIN carrying six world-space entries and a count of 3 while VRCAM's was
// empty with count 0. It lives in the upload ring and is bound in place, so it is invisible to
// CopyBufferRegion -- but CreateConstantBufferView hands us its GPU address, and the ring is
// already mapped for the cloud constants, so the bytes can be read right here.
// OFF -- the 256-byte b6 blocks turned out to be ring garbage past register 0.
extern "C" __declspec(dllexport) int32_t CyberpunkVR_CbvProbe = 1;
// Resolve a GPU virtual address inside a mapped upload heap to its CPU bytes.
static const uint8_t* upload_cpu_for_va(uint64_t va, uint64_t need) {
    std::lock_guard<std::mutex> lk(g_upload_map_mtx);
    for (uint32_t i = 0; i < g_upload_map_n; ++i) {
        const MappedUpload& m = g_upload_maps[i];
        if (!m.ptr || !m.va) continue;
        if (va >= m.va && va + need <= m.va + m.size) return m.ptr + (va - m.va);
    }
    return nullptr;
}

struct CbvSeen { uint32_t node_rva; uint32_t count[2]; uint32_t hits[2]; };
static std::array<CbvSeen, 48> g_cbv_seen{};
static uint32_t g_cbv_seen_n = 0;
static std::mutex g_cbv_mtx;

// SEH-guarded: the ring is engine memory and the block may be recycled mid-read.
static bool cbv_read_head(const uint8_t* p, float* xy, uint32_t* w) {
    __try {
        memcpy(xy, p, 8);
        float fw; memcpy(&fw, p + 12, 4);
        if (!(fw >= 0.0f && fw < 1024.0f)) return false;
        *w = static_cast<uint32_t>(fw);
        return xy[0] > 0.0f && xy[0] < 0.01f && xy[1] > 0.0f && xy[1] < 0.01f;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void cbv_probe_note(uint32_t node_rva, uint32_t count, bool vrcam) {
    bool dump = false;
    {
        std::lock_guard<std::mutex> lk(g_cbv_mtx);
        uint32_t i = 0;
        for (; i < g_cbv_seen_n; ++i) if (g_cbv_seen[i].node_rva == node_rva) break;
        if (i == g_cbv_seen_n) {
            if (g_cbv_seen_n >= g_cbv_seen.size()) return;
            g_cbv_seen[g_cbv_seen_n++] = { node_rva, {0, 0}, {0, 0} };
        }
        const int v = vrcam ? 1 : 0;
        ++g_cbv_seen[i].hits[v];
        if (count > g_cbv_seen[i].count[v]) { g_cbv_seen[i].count[v] = count; dump = true; }
    }
    if (!dump) return;
    static uint64_t s_last = 0;
    const uint64_t now = GetTickCount64();
    if (s_last && now - s_last < 10000) return;
    s_last = now;
    char line[1100];
    int u = 0;
    line[0] = 0;
    std::lock_guard<std::mutex> lk(g_cbv_mtx);
    for (uint32_t k = 0; k < g_cbv_seen_n && u < static_cast<int>(sizeof(line)) - 40; ++k) {
        const CbvSeen& c = g_cbv_seen[k];
        u += snprintf(line + u, sizeof(line) - u, "%X:max m%u/v%u (n %u/%u) ",
                      c.node_rva, c.count[0], c.count[1], c.hits[0], c.hits[1]);
    }
    log("[cbv] 256B view-constant blocks, peak count field per node: %s", line);
}

// One node's block, both views, in full. The peak-count table says WHERE the views disagree;
// this says HOW. Live-settable so the next candidate costs no rebuild.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_CbvDumpNode = 0x77AAE0;  // LightChannelVolumes
static std::mutex g_cbvd_mtx;
static uint8_t g_cbvd[2][256];
static bool    g_cbvd_have[2] = {false, false};

static void cbv_dump_note(const uint8_t* p, bool vrcam) {
    uint8_t tmp[256];
    if (!cloud_cb_raw_copy(tmp, p, 256)) return;
    bool both = false;
    {
        std::lock_guard<std::mutex> lk(g_cbvd_mtx);
        const int v = vrcam ? 1 : 0;
        if (g_cbvd_have[v]) return;                 // first block per view is enough
        memcpy(g_cbvd[v], tmp, 256);
        g_cbvd_have[v] = true;
        both = g_cbvd_have[0] && g_cbvd_have[1];
    }
    if (!both) return;
    for (int half = 0; half < 2; ++half) {
        char line[1400];
        int u = 0;
        line[0] = 0;
        for (int r = half * 8; r < half * 8 + 8; ++r) {
            float fm[4], fv[4];
            memcpy(fm, g_cbvd[0] + r * 16, 16);
            memcpy(fv, g_cbvd[1] + r * 16, 16);
            if (u < static_cast<int>(sizeof(line)) - 170)
                u += snprintf(line + u, sizeof(line) - u,
                              "[%d] M(%.5g %.5g %.5g %.5g) V(%.5g %.5g %.5g %.5g)  ", r,
                              fm[0], fm[1], fm[2], fm[3], fv[0], fv[1], fv[2], fv[3]);
        }
        log("[cbvdump] node %X regs %d-%d: %s", CyberpunkVR_CbvDumpNode,
            half * 8, half * 8 + 7, line);
    }
}

// OFF: a mutex and an 848-byte memcpy on every constant-buffer view over 768 bytes. The stage
// counters logged 114195 of those in ten seconds.
extern "C" __declspec(dllexport) int32_t CyberpunkVR_CamCbProbe = 1;
static const uint8_t* filled_cpu_for_va(uint64_t, uint64_t);   // defined with the sight probe

// SEH cannot share a frame with objects that unwind, and the reader below holds a lock and uses
// lambdas -- hence the split, same as filled_note_guarded and pso_stream_find.
static bool camcb_read_guarded(const uint8_t* cp, float* out, size_t bytes) {
    __try { memcpy(out, cp, bytes); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

// Counters per stage: a probe that finds nothing must be able to say WHERE it stopped.
static std::atomic<uint64_t> g_cc_big{0}, g_cc_cpu{0}, g_cc_basis{0}, g_cc_pos{0}, g_cc_res{0};

// PAIR DETECTION, not attribution. Two things came out of the first run and both are settled
// here rather than argued about:
//
//   * the VRCAM flag was 0 on every single accepted entry -- descriptor creation does not happen
//     inside node dispatch, so `t_vrcam_node_active` cannot say which view a constant buffer
//     belongs to. It is not consulted any more.
//   * a five-second window mixes frames, so head motion produced eight "distinct" cameras that
//     were really one camera at eight instants.
//
// What identifies the eye pair needs neither: two camera buffers built within one frame of each
// other, a plausible eye separation apart. Head motion between frames is millimetres and cannot
// counterfeit 65 mm; the eye separation is constant and cannot be confused with drift.
static float g_cc_prev_pos[3]{};
static float g_cc_prev_basis[9]{};
static float g_cc_prev_proj[9]{};       // rows 28, 29, 31 of the view-projection
static uint64_t g_cc_prev_tick = 0;
static bool g_cc_prev_valid = false;
static std::mutex g_camcb_mtx;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_CamPairMaxMs = 40;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugCamPairs = 0;

static void camcb_stages() {
    static uint64_t s_last = 0;
    const uint64_t now = GetTickCount64();
    if (s_last && now - s_last < 10000) return;
    s_last = now;
    log("[camcb] stages: big=%llu cpu=%llu basis=%llu pos=%llu accepted=%llu pairs=%llu",
        (unsigned long long)g_cc_big.load(), (unsigned long long)g_cc_cpu.load(),
        (unsigned long long)g_cc_basis.load(), (unsigned long long)g_cc_pos.load(),
        (unsigned long long)g_cc_res.load(),
        (unsigned long long)CyberpunkVR_DebugCamPairs);
}

// a = the earlier camera, b = the later one, both from the same frame.
// Each view's projection, reduced to the four numbers that can differ and be seen.
//
// The VP acts on camera-relative coordinates, so clip.x = dot(row28, ip) and w = dot(row31, ip).
// For a plain symmetric perspective row28 is right/tan(halfH) and has NO component along forward;
// a component along forward IS the frustum's horizontal off-centre, and an off-centre frustum in
// one eye and not the other shifts that whole eye's image sideways by a constant angle -- which
// is exactly the symptom, and unlike parallax it does not care about distance.
static void camcb_proj(const char* tag, const float* proj, const float* basis) {
    const float* r28 = proj + 0;
    const float* r29 = proj + 3;
    const float* fwd = basis + 3;
    const float sx = sqrtf(r28[0]*r28[0] + r28[1]*r28[1] + r28[2]*r28[2]);
    const float sy = sqrtf(r29[0]*r29[0] + r29[1]*r29[1] + r29[2]*r29[2]);
    const float cx = r28[0]*fwd[0] + r28[1]*fwd[1] + r28[2]*fwd[2];
    const float cy = r29[0]*fwd[0] + r29[1]*fwd[1] + r29[2]*fwd[2];
    log("[campair] %s proj: sx=%.6f sy=%.6f  hfov=%.4f vfov=%.4f deg  offCentre x=%+.6f y=%+.6f",
        tag, sx, sy,
        (sx > 1e-6f) ? (2.0f * atanf(1.0f / sx) * 57.29578f) : 0.0f,
        (sy > 1e-6f) ? (2.0f * atanf(1.0f / sy) * 57.29578f) : 0.0f,
        cx, cy);
}

static void camcb_pair(const float* ap, const float* ab, const float* bp, const float* bb,
                       const float* aproj, const float* bproj) {
    static uint64_t s_last = 0;
    const uint64_t now = GetTickCount64();
    if (s_last && now - s_last < 3000) return;
    s_last = now;
    const float d[3] = { bp[0] - ap[0], bp[1] - ap[1], bp[2] - ap[2] };
    // Decomposed in the FIRST camera's own basis: rows 40/41/42 are right / forward / up
    // (verified by right x forward = up on a live dump). A correct eye separation is
    // (+-IPD, 0, 0) here; anything in the forward or up column is the separation going somewhere
    // it should not, and that is a stereo bug independent of the sight.
    const float dr = d[0]*ab[0] + d[1]*ab[1] + d[2]*ab[2];
    const float df = d[0]*ab[3] + d[1]*ab[4] + d[2]*ab[5];
    const float du = d[0]*ab[6] + d[1]*ab[7] + d[2]*ab[8];
    log("[campair] A=(%.5f %.5f %.5f)  B=(%.5f %.5f %.5f)", ap[0], ap[1], ap[2], bp[0], bp[1], bp[2]);
    log("[campair] delta=(%.5f %.5f %.5f) |d|=%.4f m  ->  right=%+.4f forward=%+.4f up=%+.4f",
        d[0], d[1], d[2], sqrtf(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]), dr, df, du);
    log("[campair] A right=(%+.6f %+.6f %+.6f) fwd=(%+.6f %+.6f %+.6f)",
        ab[0], ab[1], ab[2], ab[3], ab[4], ab[5]);
    auto ang = [](const float* x, const float* y) {
        float t = x[0]*y[0] + x[1]*y[1] + x[2]*y[2];
        if (t > 1.0f) t = 1.0f;
        if (t < -1.0f) t = -1.0f;
        return acosf(t) * 1000.0f;                     // milliradians
    };
    log("[campair] axis disagreement: right=%.3f forward=%.3f up=%.3f mrad",
        ang(ab + 0, bb + 0), ang(ab + 3, bb + 3), ang(ab + 6, bb + 6));
    camcb_proj("A", aproj, ab);
    camcb_proj("B", bproj, bb);
}

static void camcb_note(const uint8_t* cp, bool vrcam) {
    g_cc_cpu.fetch_add(1, std::memory_order_relaxed);
    float r[53 * 4];
    if (!camcb_read_guarded(cp, r, sizeof(r))) { camcb_stages(); return; }
    const float* p36 = r + 36 * 4;
    const float* p37 = r + 37 * 4;
    const float* p47 = r + 47 * 4;
    const float* b0 = r + 40 * 4;
    const float* b1 = r + 41 * 4;
    const float* b2 = r + 42 * 4;

    // Fingerprint. Four independent structural facts, because ONE of them (an orthonormal 3x3 at
    // this offset) matched 8303 unrelated buffers -- object transforms and skinning palettes are
    // full of those, and a thousand of them also had a plausible size in row 47.
    auto unit = [](const float* v) {
        const float n = v[0]*v[0] + v[1]*v[1] + v[2]*v[2];
        return n > 0.98f && n < 1.02f;
    };
    const float d01 = b0[0]*b1[0] + b0[1]*b1[1] + b0[2]*b1[2];
    const float d02 = b0[0]*b2[0] + b0[1]*b2[1] + b0[2]*b2[2];
    const float d12 = b1[0]*b2[0] + b1[1]*b2[1] + b1[2]*b2[2];
    if (!unit(b0) || !unit(b1) || !unit(b2) ||
        fabsf(d01) > 0.02f || fabsf(d02) > 0.02f || fabsf(d12) > 0.02f) { camcb_stages(); return; }
    g_cc_basis.fetch_add(1, std::memory_order_relaxed);

    // Position AND rebase origin, and the fact that they coincide. The engine rebases the world
    // on the camera, so [36].xyz == [37].xyz by construction, with [36].w = 0 and [37].w = 1.
    // Dropping this clause -- on the theory that it was an artefact of a session-less capture --
    // is exactly what let a thousand strangers through.
    if (p37[3] != 1.0f || p36[3] != 0.0f) { camcb_stages(); return; }
    if (fabsf(p36[0] - p37[0]) > 1e-3f || fabsf(p36[1] - p37[1]) > 1e-3f ||
        fabsf(p36[2] - p37[2]) > 1e-3f) { camcb_stages(); return; }
    if (fabsf(p36[0]) + fabsf(p36[1]) + fabsf(p36[2]) < 1.0f) { camcb_stages(); return; }
    g_cc_pos.fetch_add(1, std::memory_order_relaxed);

    // The view's pixel size: whole numbers in range, with .zw exactly (0, 1).
    if (p47[2] != 0.0f || p47[3] != 1.0f) { camcb_stages(); return; }
    if (!(p47[0] >= 256.0f && p47[0] <= 16384.0f && p47[1] >= 256.0f && p47[1] <= 16384.0f) ||
        p47[0] != floorf(p47[0]) || p47[1] != floorf(p47[1])) { camcb_stages(); return; }
    g_cc_res.fetch_add(1, std::memory_order_relaxed);

    // Small auxiliary views (256x256 probes at a different altitude showed up in the first run)
    // are not eyes; requiring a full-size view keeps them out without naming them.
    if (p47[0] < 1024.0f || p47[1] < 1024.0f) { camcb_stages(); return; }

    float basis[9];
    memcpy(basis + 0, b0, 12);
    memcpy(basis + 3, b1, 12);
    memcpy(basis + 6, b2, 12);
    float proj[9];
    memcpy(proj + 0, r + 28 * 4, 12);
    memcpy(proj + 3, r + 29 * 4, 12);
    memcpy(proj + 6, r + 31 * 4, 12);
    float prevPos[3], prevBasis[9], prevProj[9];
    bool pair = false;
    {
        std::lock_guard<std::mutex> lk(g_camcb_mtx);
        const uint64_t now = GetTickCount64();
        if (g_cc_prev_valid && now - g_cc_prev_tick <= CyberpunkVR_CamPairMaxMs) {
            const float dx = p36[0] - g_cc_prev_pos[0];
            const float dy = p36[1] - g_cc_prev_pos[1];
            const float dz = p36[2] - g_cc_prev_pos[2];
            const float dd = sqrtf(dx*dx + dy*dy + dz*dz);
            if (dd > 0.02f && dd < 0.15f) {
                memcpy(prevPos, g_cc_prev_pos, sizeof(prevPos));
                memcpy(prevBasis, g_cc_prev_basis, sizeof(prevBasis));
                memcpy(prevProj, g_cc_prev_proj, sizeof(prevProj));
                pair = true;
                ++CyberpunkVR_DebugCamPairs;
            }
        }
        memcpy(g_cc_prev_pos, p36, 12);
        memcpy(g_cc_prev_basis, basis, sizeof(basis));
        memcpy(g_cc_prev_proj, proj, sizeof(proj));
        g_cc_prev_tick = now;
        g_cc_prev_valid = true;
    }
    if (pair) camcb_pair(prevPos, prevBasis, p36, basis, prevProj, proj);
    camcb_stages();
}

static void STDMETHODCALLTYPE hk_CreateCBV(ID3D12Device* self,
        const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dst) {
    g_orig_CreateCBV(self, desc, dst);
    // ---- the camera constant buffer, per view -------------------------------------------------
    // The one number the whole sight question now turns on: where each view's camera actually is,
    // read on the GPU side at the same instant as the weapon's world position.
    //
    // Why here: b1 is bound straight out of the upload ring, so no CopyBufferRegion ever carries
    // it -- the same reason the HUD composite's constants had to be found by scanning. But
    // CreateConstantBufferView hands us its GPU address, and the ring is already mapped.
    //
    // Identified by CONTENT, not by size: rows 40..42 are an orthonormal basis, row 37.w is
    // exactly 1.0, and row 36 equals row 37. Nothing else in the engine's constant traffic looks
    // like that, and it is true at any resolution.
    // No counter on the unfiltered path: this site fires ~32 million times a session, and an
    // atomic on every one of them is real frame time spent on nothing.
    if (CyberpunkVR_CamCbProbe && desc) {
        if (desc->SizeInBytes >= 768) {
            g_cc_big.fetch_add(1, std::memory_order_relaxed);
            // Two routes to the bytes: the ring if it is mapped, else the record of what was
            // copied into it. Neither is guaranteed for a buffer bound straight out of the ring,
            // which is exactly why the counters above exist.
            const uint8_t* cp = upload_cpu_for_va(desc->BufferLocation, 848);
            if (!cp) cp = filled_cpu_for_va(desc->BufferLocation, 848);
            if (cp) camcb_note(cp, t_vrcam_node_active);
            else camcb_stages();
        }
    }
    // Grading-LUT capture first: it wants every CBV made inside GenerateTonemappingLUT, of any
    // size, not just the 256-byte view blocks the older probe filtered for.
    if (CyberpunkVR_GradeCbProbe && t_grade_cb_view >= 0 && desc &&
        desc->SizeInBytes && desc->SizeInBytes <= GRADE_CB_MAX &&
        t_grade_cb_idx < GRADE_CB_SLOTS) {
        const uint8_t* gp = upload_cpu_for_va(desc->BufferLocation, desc->SizeInBytes);
        if (gp) {
            uint8_t tmp[GRADE_CB_MAX];
            if (cloud_cb_raw_copy(tmp, gp, desc->SizeInBytes)) {
                const int v = t_grade_cb_view;
                const uint32_t k = t_grade_cb_idx;
                std::lock_guard<std::mutex> lk(g_gcb_mtx);
                memcpy(g_gcb[v][k], tmp, desc->SizeInBytes);
                g_gcb_len[v][k] = desc->SizeInBytes;
            }
            ++t_grade_cb_idx;
        }
    }
    if (!CyberpunkVR_CbvProbe || !desc || desc->SizeInBytes != 256 || !g_exe_base) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(g_exe_base);
    const uintptr_t work = t_current_node_work;
    if (work <= base) return;
    const uint8_t* p = upload_cpu_for_va(desc->BufferLocation, 256);
    if (!p) return;
    float xy[2]; uint32_t cnt = 0;
    if (!cbv_read_head(p, xy, &cnt)) return;     // not a view-constant block
    const uint32_t rva = static_cast<uint32_t>(work - base);
    cbv_probe_note(rva, cnt, t_vrcam_node_active);
    if (rva == CyberpunkVR_CbvDumpNode) cbv_dump_note(p, t_vrcam_node_active);
}

static void STDMETHODCALLTYPE hk_CreateRTV(ID3D12Device* self, ID3D12Resource* res,
        const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dst) {
    g_orig_CreateRTV(self, res, desc, dst);
    rtv_dim_register(dst, res);   // broad map (any format) for the crop-blit RT-size probe
    hud_register_rtv(res, desc, dst);
    if (desc) {
        mirror_register_rtv(res, desc->Format, dst);
    } else if (res) {
        D3D12_RESOURCE_DESC resource_desc{};
        if (mirror_get_resource_desc(res, &resource_desc))
            mirror_register_rtv(res, resource_desc.Format, dst);
    }
}

// Lazily create a committed texture matching `src`'s desc (in RENDER_TARGET) + an RTV in
// our own heap, so the engine can render the vrcam final directly into it. One target
// (single vrcam view for now). Returns the RTV handle, {0} on failure.
static D3D12_CPU_DESCRIPTOR_HANDLE mirror_ensure_own_target_rtv(
        ID3D12Resource* src, DXGI_FORMAT view_format) {
    D3D12_CPU_DESCRIPTOR_HANDLE none{0};
    if (!g_game_device || !src) return none;
    std::lock_guard<std::mutex> lk(g_own_target_mtx);
    if (g_own_target) return g_own_rtv;
    D3D12_RESOURCE_DESC d{};
    if (!mirror_get_resource_desc(src, &d)) return none;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
    d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource* tex = nullptr;
    if (FAILED(g_game_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&tex))) || !tex)
        return none;
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    ID3D12DescriptorHeap* heap = nullptr;
    if (FAILED(g_game_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap))) || !heap) {
        tex->Release(); return none;
    }
    D3D12_RENDER_TARGET_VIEW_DESC rd2{};
    rd2.Format = (view_format != DXGI_FORMAT_UNKNOWN) ? view_format : d.Format;
    rd2.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
    g_game_device->CreateRenderTargetView(tex, &rd2, h);
    tex->SetName(L"CyberpunkVR_VrcamOwnTarget");
    g_own_target = tex; g_own_rtv_heap = heap; g_own_rtv = h;
    log("[owntgt] committed vrcam target=%p %llux%u fmt=%u", tex,
        (unsigned long long)d.Width, d.Height, (unsigned)rd2.Format);
    return g_own_rtv;
}

// Naming the outline surface by DESCRIPTOR does not work: at the second view's resolution there
// are two 1-mip RGBA8_SRGB render targets with identical descs (83220 and 83328), and picking by
// desc alone is exactly the kind of guess that has cost this project several rounds. Name it by
// WHO BINDS IT instead -- the target bound while CRenderNode_RenderVisionElements is on the
// stack is the outline surface by construction, per view. That node is the sole writer, and its
// draws are attributed correctly (the draw census sees it firing for both views).
constexpr uint32_t VISION_ELEMENTS_RVA = 0x61FDE4;
// The outline layer is produced by a COMPUTE dispatch, not a draw -- which is why hanging the
// detection off OMSetRenderTargets found nothing at all. In the capture:
//     VRCAM  PipelineState_1213  Dispatch(306,320,1) -> UAV barrier on Resource_85137 (2444x2560)
//     MAIN   PipelineState_1213  Dispatch(320,320,1) -> UAV barrier on Resource_85164 (2560x2560)
// so both eyes DO produce their own, fully symmetric; it is only never composited into the
// second one. (Resource_83328, which looked like VRCAM's, is the DiscardResource target on the
// very next line -- a transient whose memory happened to hold someone else's picture.)
//
// The dispatch-shape rule alone is NOT unique -- measured, not assumed. Live it fired 30845 times
// in one session across five distinct shapes per view, and the capture says exactly why: eleven
// dispatches in the frame write a full-size RGBA8 UAV at 8x8, six different pipeline states.
// Per view, ordered:
//
//     AsyncComputeDuringShadowmaps  PS494  -> half-size    PS619 -> half-size   PS1143 -> FULL
//     PostFX                        PS1040 -> FULL                              PS1213 -> FULL
//
// PS1213 is the outline layer (VRCAM Resource_85137, MAIN Resource_85164). So two more conditions
// pin it, and both are things we can check honestly at the barrier:
//   * the texture is EXACTLY the view's own render size -- kills the half-size passes outright;
//   * within one frame-graph NODE it is the Nth full-size match, N from the capture (PS1040 is 0,
//     PS1213 is 1). The ordinal resets when the node changes, so the async-compute node's own
//     full-size write cannot bleed into the count.
// The [vismap] report below prints node/size/ordinal so the value of N is read off measurement
// rather than trusted.
static thread_local UINT t_last_disp[3] = {0, 0, 0};

// MEASURED, and it turns out the ordinal was the wrong axis. The [vismap] table for a live scan:
//
//   VRCAM  61EE78#0 1222x1280   61EE78#1 1222x1280   <- CRenderNode_DrawConeAO, half-res
//          EFC110#0 2444x2560                        <- CRenderNode_GenerateTonemappingLUT
//          77120C#0 2444x2560                        <- CRenderNode_GameplayPostFX
//          61FDE4#0 2444x2560                        <- CRenderNode_RenderVisionElements
//
// Three nodes produce a full-size match and every one of them at ordinal 0, so VisionPick=1 chose
// nothing at all -- which is why no snapshot was taken. But the table also names the writer
// outright: CRenderNode_RenderVisionElements, the node the outline belongs to by its own name,
// firing 1190 times for VRCAM against MAIN's 1191. It never appeared in the DRAW census because
// it does not draw; it dispatches. Select by node, and the identification stops being a heuristic.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_VisionNode = 0x61FDE4;
// The tally that FOUND that node. Report-only; the snapshot below does not need it.
extern "C" __declspec(dllexport) int32_t CyberpunkVR_VisionMap = 0;
// Ordinal of the full-size match inside that node. 0 -- measured, not assumed.
extern "C" __declspec(dllexport) int32_t CyberpunkVR_VisionPick = 0;
// Blend the snapshotted layer into the second eye (openxr_capture does the pass).
extern "C" __declspec(dllexport) int CyberpunkVR_VisionToSecondEye = 1;
// Blend for the outline layer: 3 = straight alpha, which is what PipelineState_1216 does and the
// default; 0 = premultiplied, 1 = opaque replace, 2 = additive -- kept for A/B without a rebuild.
extern "C" __declspec(dllexport) int CyberpunkVR_VisionDebug = 3;
// The outline layer is the size of the view's RENDER RECT (2444x2560), while the eye image is the
// texture the engine copies it into (2444x2444) -- the top 2444 rows of it. Stretching the layer
// over the eye therefore lifts it by 116 rows at the bottom, which is the "outline sits higher
// than MAIN's" symptom exactly. 1 = pixel-exact (correct), 0 = old stretch, for A/B.
extern "C" __declspec(dllexport) int   CyberpunkVR_VisionFit  = 1;
extern "C" __declspec(dllexport) float CyberpunkVR_VisionOffX = 0.0f;
extern "C" __declspec(dllexport) float CyberpunkVR_VisionOffY = 0.0f;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugVisionOverlays = 0;

static bool vision_layer_signature(const D3D12_RESOURCE_DESC& d) {
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
           d.MipLevels == 1 && d.DepthOrArraySize == 1 && d.SampleDesc.Count == 1 &&
           d.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
           (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
           (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) &&
           d.Width >= 1000 && d.Height >= 1000;
}

static bool vision_matches_last_dispatch(const D3D12_RESOURCE_DESC& d) {
    if (t_last_disp[2] != 1 || !t_last_disp[0] || !t_last_disp[1]) return false;
    const UINT gx = static_cast<UINT>((d.Width + 7) / 8);
    const UINT gy = (d.Height + 7) / 8;
    return t_last_disp[0] == gx && t_last_disp[1] == gy;
}

// Is this the second view's OWN full-size surface, rather than one of its half-res intermediates?
static bool vision_is_vrcam_full_size(const D3D12_RESOURCE_DESC& d) {
    const uint32_t w = g_vrcam_view_w.load(std::memory_order_acquire);
    const uint32_t h = g_vrcam_view_h.load(std::memory_order_acquire);
    return w && h && d.Width == w && d.Height == h;
}

// Per-node ordinal of full-size matches, on the recording thread.
static thread_local uintptr_t t_vision_node = 0;
static thread_local int32_t   t_vision_ord = 0;

static thread_local ID3D12Resource* t_vision_bound = nullptr;

// ---- node -> render-target map ---------------------------------------------------------------
// Built because two inferences in a row went wrong here. "RenderVisionElements draws for both
// views" came from its ABSENCE in the exclusive draw census -- but absence there also means
// "never draws at all", which is what the missing [vision] lines then showed. Rather than guess
// again, record what every node actually binds, per view: node RVA + target size/format/mips.
// The scanner's outline surface is known from a capture (MAIN 2560x2560 1-mip RGBA8_UNORM,
// VRCAM 2444x2560 1-mip RGBA8_UNORM_SRGB), so whichever node binds that shape is its writer.
extern "C" __declspec(dllexport) int32_t CyberpunkVR_RtMapProbe = 1;   // OFF: [rtmap] node -> render-target map
struct RtMapEntry {
    uint32_t node_rva, w, h, fmt, mips;
    uint64_t hits[2];
};
static std::array<RtMapEntry, 128> g_rtmap{};
static uint32_t g_rtmap_n = 0;
static std::mutex g_rtmap_mtx;

static void rtmap_report() {
    static uint64_t s_last = 0;
    const uint64_t now = GetTickCount64();
    if (s_last && now - s_last < 20000) return;
    RtMapEntry e[128];
    uint32_t n;
    {
        std::lock_guard<std::mutex> lk(g_rtmap_mtx);
        n = g_rtmap_n;
        for (uint32_t i = 0; i < n; ++i) e[i] = g_rtmap[i];
    }
    if (!n) return;
    s_last = now;
    // Only 1-mip colour targets at view size or larger: that is the shape of an overlay layer,
    // and printing every shadow atlas and froxel grid would drown the line.
    for (int pass = 0; pass < 2; ++pass) {
        char line[1400];
        int u = 0, c = 0;
        line[0] = 0;
        for (uint32_t i = 0; i < n; ++i) {
            if (!e[i].hits[pass]) continue;
            // RGBA8 flavours only (TYPELESS 27 / UNORM 28 / UNORM_SRGB 29). The first report
            // listed every full-size 1-mip target and ran past the line budget, which is how a
            // 37-entry list arrived truncated. The outline surface is RGBA8, so this both
            // shortens the line and keeps exactly the candidates.
            if (e[i].mips != 1 || e[i].w < 1000) continue;
            if (e[i].fmt != 27 && e[i].fmt != 28 && e[i].fmt != 29) continue;
            ++c;
            if (u < static_cast<int>(sizeof(line)) - 56)
                u += snprintf(line + u, sizeof(line) - u, "%X:%ux%u/f%u(%llu) ",
                              e[i].node_rva, e[i].w, e[i].h, e[i].fmt,
                              (unsigned long long)e[i].hits[pass]);
        }
        log("[rtmap] %-5s node:RGBA8 1-mip full-size targets (%d): %s",
            pass ? "VRCAM" : "MAIN", c, c ? line : "(none)");
    }
}

static void rtmap_note(ID3D12Resource* res, bool vrcam) {
    if (!g_exe_base || !res) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(g_exe_base);
    const uintptr_t work = t_current_node_work;
    const uint32_t rva = (work > base) ? static_cast<uint32_t>(work - base) : 0;
    D3D12_RESOURCE_DESC d{};
    if (!mirror_get_resource_desc(res, &d)) return;
    {
        std::lock_guard<std::mutex> lk(g_rtmap_mtx);
        uint32_t i = 0;
        for (; i < g_rtmap_n; ++i)
            if (g_rtmap[i].node_rva == rva && g_rtmap[i].w == (uint32_t)d.Width &&
                g_rtmap[i].h == d.Height && g_rtmap[i].fmt == (uint32_t)d.Format &&
                g_rtmap[i].mips == d.MipLevels) break;
        if (i == g_rtmap_n) {
            if (g_rtmap_n >= g_rtmap.size()) return;
            g_rtmap[g_rtmap_n++] = { rva, (uint32_t)d.Width, d.Height,
                                     (uint32_t)d.Format, d.MipLevels, {0, 0} };
        }
        ++g_rtmap[i].hits[vrcam ? 1 : 0];
    }
    rtmap_report();
}

// Aggregate, not one line per hit. The first version logged on every change of resource pointer,
// which the engine's transient ring makes near-continuous: 30845 lines and a 2.2 MB log for one
// scan. Same mistake shape as the truncated [rtmap] line -- report a table, not a stream.
struct VisMapEntry {
    uint32_t node_rva, w, h;
    int32_t  ord;
    uint64_t hits[2];
};
static std::array<VisMapEntry, 64> g_vismap{};
static uint32_t g_vismap_n = 0;
static std::mutex g_vismap_mtx;

static void vision_report() {
    static uint64_t s_last = 0;
    const uint64_t now = GetTickCount64();
    if (s_last && now - s_last < 20000) return;
    VisMapEntry e[64];
    uint32_t n;
    {
        std::lock_guard<std::mutex> lk(g_vismap_mtx);
        n = g_vismap_n;
        for (uint32_t i = 0; i < n; ++i) e[i] = g_vismap[i];
    }
    if (!n) return;
    s_last = now;
    for (int pass = 0; pass < 2; ++pass) {
        char line[1200];
        int u = 0, c = 0;
        line[0] = 0;
        for (uint32_t i = 0; i < n; ++i) {
            if (!e[i].hits[pass]) continue;
            ++c;
            if (u < static_cast<int>(sizeof(line)) - 48)
                u += snprintf(line + u, sizeof(line) - u, "%X#%d:%ux%u(%llu) ",
                              e[i].node_rva, e[i].ord, e[i].w, e[i].h,
                              (unsigned long long)e[i].hits[pass]);
        }
        log("[vismap] %-5s node#ordinal:size(hits) (%d): %s",
            pass ? "VRCAM" : "MAIN", c, c ? line : "(none)");
    }
}

static void vision_note_surface(ID3D12Resource* res, bool vrcam,
                                uint32_t node_rva, int32_t ord) {
    D3D12_RESOURCE_DESC d{};
    if (!mirror_get_resource_desc(res, &d)) return;
    {
        std::lock_guard<std::mutex> lk(g_vismap_mtx);
        uint32_t i = 0;
        for (; i < g_vismap_n; ++i)
            if (g_vismap[i].node_rva == node_rva && g_vismap[i].ord == ord &&
                g_vismap[i].w == (uint32_t)d.Width && g_vismap[i].h == d.Height) break;
        if (i == g_vismap_n) {
            if (g_vismap_n >= g_vismap.size()) return;
            g_vismap[g_vismap_n++] = { node_rva, (uint32_t)d.Width, d.Height, ord, {0, 0} };
        }
        ++g_vismap[i].hits[vrcam ? 1 : 0];
    }
    vision_report();
}

static void STDMETHODCALLTYPE hk_OMSetRenderTargets(
        ID3D12GraphicsCommandList* self, UINT count,
        const D3D12_CPU_DESCRIPTOR_HANDLE* handles, BOOL contiguous,
        const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
    PFN_OMSetRenderTargets original = command_list_original_om(self);
    if (!original) return;
    // Which frame-graph node is binding, and what it is binding. Deliberately NOT gated on any
    // one probe: the HUD surface is identified here, and that is a feature, not diagnostics.
    if (g_exe_base && handles && count >= 1) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(g_exe_base);
        const uintptr_t work = t_current_node_work;
        const uint32_t rva = (work > base) ? static_cast<uint32_t>(work - base) : 0;
        ID3D12Resource* bound = nullptr;
        const bool hud_node = CyberpunkVR_HudByNode &&
            (rva == CyberpunkVR_HudNodeRva || rva == CyberpunkVR_HudNodeRva2);
        if (CyberpunkVR_RtMapProbe || CyberpunkVR_HudNodeProbe || hud_node) {
            __try { bound = rtv_resource_lookup(handles[0].ptr); }
            __except (EXCEPTION_EXECUTE_HANDLER) { bound = nullptr; }
        }
        if (CyberpunkVR_RtMapProbe && bound) rtmap_note(bound, t_vrcam_node_active);
        if (hud_node) {
            g_hud_node_binds.fetch_add(1, std::memory_order_relaxed);
            if (!bound) g_hud_node_unresolved.fetch_add(1, std::memory_order_relaxed);
        }
        if (hud_node && bound) hud_adopt_by_node(bound);
        if (CyberpunkVR_HudNodeProbe && bound) hud_node_note(bound, t_vrcam_node_active);
    }
    // Track the primary bound RT dims for THIS recording thread, so the RSSetViewports/ScissorRects
    // hooks can detect the crop pass (render-res viewport on an OUTPUT-size RT) reliably -- both run
    // consecutively on the SAME thread, no dependency on which thread ran the DLSS eval.
    if (handles && count >= 1) {
        __try {
            uint32_t rw = 0, rh = 0;
            if (rtv_dim_lookup(handles[0].ptr, &rw, &rh)) {
                t_cur_rt_w = rw; t_cur_rt_h = rh;
            } else { t_cur_rt_w = 0; t_cur_rt_h = 0; }
        } __except (EXCEPTION_EXECUTE_HANDLER) { t_cur_rt_w = 0; t_cur_rt_h = 0; }
    }
    // Redirect the vrcam-final RTV to our own committed target: the engine then renders
    // the final into a stable, never-aliased resource we control (kills the shared-heap
    // race where the mirror read main's content). Only inside the ctx-keyed vrcam
    // RenderFinal2D node (view-aware) and only for the vrcam-dims target (mirror_find_bound_rtv
    // returns only dims-filtered candidates).
    // (The HUD snapshot used to be triggered here, at the unbind of the HUD's mip-0 target. That
    //  is too early: the glow mips do not exist yet. It now hangs off the barrier that releases
    //  the last mip -- see hk_ResourceBarrier.)

    D3D12_CPU_DESCRIPTOR_HANDLE sub[8];
    const D3D12_CPU_DESCRIPTOR_HANDLE* use = handles;
    BOOL use_contig = contiguous;
    bool subbed = false;
    if (CyberpunkVR_VrcamOwnTarget && t_mirror_copy_node_active && handles && count &&
            count <= 8 && g_game_device) {
        __try {
            const UINT inc = g_game_device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            for (UINT i = 0; i < count; ++i) {
                sub[i].ptr = contiguous ? handles[0].ptr + (SIZE_T)inc * i
                                        : handles[i].ptr;
                DXGI_FORMAT vf = DXGI_FORMAT_UNKNOWN;
                ID3D12Resource* res = mirror_find_bound_rtv(sub[i].ptr, &vf);
                if (res) {
                    D3D12_CPU_DESCRIPTOR_HANDLE own =
                        mirror_ensure_own_target_rtv(res, vf);
                    if (own.ptr) { sub[i] = own; subbed = true; }
                }
            }
            if (subbed) { use = sub; use_contig = FALSE; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            subbed = false; use = handles; use_contig = contiguous;
        }
    }
    original(self, count, use, use_contig, depth);
    if (subbed) {
        ++CyberpunkVR_DebugOwnTargetSubs;
        if (g_own_target)
            g_captured_vrcam_res.store(g_own_target, std::memory_order_release);
    }
    // 2-MRT probe: the vrcam post-DLSS pass binds {transient RT0, persistent RT1} where
    // RT1's descriptor PING-PONGS A/B/A across frames (Nsight 3-frame diff; main's pair
    // is constant). RT0 resolves via the dims-filtered candidates (vrcam-res only, so
    // main's 2-RT pass self-filters out). Log the first occurrences to identify the
    // ping-pong pair in the live session; count all hits.
    bool is_2rt_bind = false;
    if (count == 2 && handles && g_game_device) {
        const UINT inc2 = g_game_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        const SIZE_T h0 = handles[0].ptr;
        const SIZE_T h1 = contiguous ? handles[0].ptr + inc2 : handles[1].ptr;
        DXGI_FORMAT vf0 = DXGI_FORMAT_UNKNOWN;
        ID3D12Resource* rt0 = mirror_find_bound_rtv(h0, &vf0);
        if (rt0) {
            is_2rt_bind = true;
            InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(
                &CyberpunkVR_Debug2RtBinds));
            // Tonemap identification: RT1 changed for a known RT0 => ping-pong pass.
            int slot = -1;
            for (int i = 0; i < 4; ++i) {
                const SIZE_T seen =
                    g_2rt_seen_h0[i].load(std::memory_order_acquire);
                if (seen == h0) { slot = i; break; }
                if (!seen) {
                    SIZE_T want = 0;
                    if (g_2rt_seen_h0[i].compare_exchange_strong(want, h0)) {
                        g_2rt_seen_h1[i].store(h1, std::memory_order_release);
                        slot = -2;      // just inserted; no history yet
                    } else if (g_2rt_seen_h0[i].load(std::memory_order_acquire)
                               == h0) {
                        slot = i;
                    }
                    if (slot != -1) break;
                }
            }
            if (slot >= 0 &&
                g_2rt_seen_h1[slot].exchange(h1, std::memory_order_acq_rel) != h1) {
                SIZE_T want = 0;
                g_tonemap_h0.compare_exchange_strong(want, h0);   // identify tonemap pass ([2rt] log removed)
            }
            // CB-probe window (diagnostic) still keyed on the ping-pong h0.
            if (g_tonemap_h0.load(std::memory_order_relaxed) == h0) {
                t_in_vrcam_2rt = true;
                t_2rt_cb_armed = true;
            }
            // Tonemap RT0 capture keyed on WORK-RVA 0x768510 (STABLE across frames),
            // NOT the per-frame descriptor handle h0 (which stopped matching after ~16
            // frames and froze the snapshot). RT0 = the tonemapped color OUTPUT (proven
            // by the full reverse: DLSS writes raw linear to post-color at an early gen;
            // tonemap writes the correct dark result; RTT-Final2D reads the early gen).
            // The node epilogue snapshots this into committed g_stable_tex.
            if (t_current_node_work ==
                    reinterpret_cast<uintptr_t>(g_exe_base) + TONEMAP_WORK_RVA) {
                t_tm_rt0 = rt0;
                t_tm_rt0_list = self;
                t_tm_rt0_state = (uint32_t)D3D12_RESOURCE_STATE_RENDER_TARGET;
                t_tm_consumed = false;   // arm the epilogue snapshot
            }
            // ([2rt] per-bind diagnostic logging removed)
        }
    }
    if (!is_2rt_bind) t_in_vrcam_2rt = false;   // any other bind closes the window
    if (!t_mirror_copy_node_active || !handles || !count) return;
    const UINT increment = g_game_device
        ? g_game_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV) : 0;
    for (UINT i = 0; i < count; ++i) {
        const SIZE_T handle = contiguous
            ? handles[0].ptr + static_cast<SIZE_T>(increment) * i
            : handles[i].ptr;
        DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN;
        ID3D12Resource* resource =
            mirror_find_bound_rtv(handle, &view_format);
        // ---- RTV-pick diagnostic ---------------------------------------------------
        // Which render target the mirror latches decides what the VRCAM eye shows, and
        // the pick is dims-filtered: it assumes nothing else in the frame is the VRCAM
        // size. With the MAIN resolution override that assumption is no longer safe --
        // MAIN is forced to the SAME square size as VRCAM. On top of that this loop
        // keeps the LAST matching target of a multi-RT bind, so a 2-MRT vrcam pass
        // whose second target now also passes the filter would latch the wrong one.
        // Log the first binds so the actual pick is a fact, not a guess.
        if (CyberpunkVR_DebugRtvPickLog > 0) {
            --CyberpunkVR_DebugRtvPickLog;
            D3D12_RESOURCE_DESC rd{};
            // "not-a-candidate" on its own does not say WHY, and why is the whole question once
            // the node has started binding something we do not know. The broad RTV->resource map
            // is filled by the same CreateRenderTargetView hook with no format or size filter at
            // all, so it answers it: present there but missing from the candidate table means
            // mirror_register_rtv REFUSED it, and the dims and format printed are the ones it
            // refused. Missing from both means we never saw that descriptor created.
            //
            // Reported through a SEPARATE local. `resource` is the pick this loop acts on; writing
            // the diagnostic's answer back into it would turn a log line into a behaviour change.
            ID3D12Resource* shown = resource;
            const char* via = "";
            if (!shown) {
                shown = rtv_resource_lookup(handle);
                via = shown ? " [refused by the candidate filter]"
                            : " [descriptor never seen created]";
            }
            const bool got = shown && mirror_get_resource_desc(shown, &rd);
            log("[rtvpick] bind i=%u/%u handle=%p -> res=%p %s%llux%u fmt=%u view=%u %s%s",
                i, count, (void*)handle, (void*)shown,
                got ? "" : "(no desc) ",
                got ? (unsigned long long)rd.Width : 0ull,
                got ? rd.Height : 0u,
                got ? (unsigned)rd.Format : 0u,
                (unsigned)view_format,
                resource ? "MATCH-latched" : "not-a-candidate",
                via);
        }
        if (resource) {
            ++CyberpunkVR_DebugMirrorRtvHits;
            t_mirror_copy_rtv = resource;
            t_mirror_copy_rtv_format = view_format;
            // Engine activated this target with ALIASING + ->RENDER_TARGET + Discard
            // right before binding it (ev95193<ev95195); track further transitions
            // in hk_ResourceBarrier so the inline copy uses the exact current state.
            t_mirror_copy_list = self;
            t_mirror_src_state = (uint32_t)D3D12_RESOURCE_STATE_RENDER_TARGET;
            // This command list runs the vrcam CopyToTexture blit -> trigger the
            // 11on12 copy only when the game SUBMITS it (correct frame contents).
            g_mirror_pending_list.store(self, std::memory_order_release);
        }
    }
}

static bool mirror_stage_desc_matches(const D3D12_RESOURCE_DESC& a,
                                      const D3D12_RESOURCE_DESC& b) {
    return a.Dimension == b.Dimension && a.Width == b.Width &&
        a.Height == b.Height && a.DepthOrArraySize == b.DepthOrArraySize &&
        a.MipLevels == b.MipLevels && a.Format == b.Format &&
        a.SampleDesc.Count == b.SampleDesc.Count &&
        a.SampleDesc.Quality == b.SampleDesc.Quality;
}


// Record, INTO THE ENGINE'S OWN command list, a copy of the freshly written vrcam
// final into our committed g_stable_tex. Called at the end of the vrcam RenderFinal2D
// node: the composite draw is already recorded, no later pass is recorded yet, so in
// queue order the copy executes after the final write and BEFORE any ALIASING barrier
// re-purposes the transient's heap memory (the proven bright/dark root). No SEH here:
// same risk class as the existing raw-vtable append path (uses C++ locks => C2712).
static void mirror_stable_inline_copy(ID3D12GraphicsCommandList* list,
        ID3D12Resource* src, uint32_t src_state) {
    if (!g_game_device || !list || !src) return;
    const CommandListVtableHook* e = command_list_hook_entry(list);
    D3D12_RESOURCE_DESC d{};
    if (!e || !e->barrier_call || !e->copyres || !mirror_get_resource_desc(src, &d)) {
        InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(
            &CyberpunkVR_DebugStableSkips));
        return;
    }
    ID3D12Resource* stable = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_stable_mtx);
        if (g_stable_tex && !mirror_stage_desc_matches(g_stable_desc, d)) {
            // Resolution changed: old snapshot may still be referenced by an in-flight
            // deferred copy -> intentionally leak it (rare, dev-time only) and recreate.
            g_stable_tex = nullptr;
        }
        if (!g_stable_tex) {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            hp.CreationNodeMask = 1;
            hp.VisibleNodeMask = 1;
            D3D12_RESOURCE_DESC nd = d;
            // Plain copy target -- no ALLOW_SIMULTANEOUS_ACCESS.
            //
            // I added that flag chasing the first device hang, before the real cause turned
            // out to be a typeless RTV elsewhere. It does not belong here and it is not free:
            // d12_mirror_ensure() builds the mirror texture from THIS desc, so the flag was
            // inherited by the surface the mirror shares with D3D11, and enabling the mirror
            // died with DXGI_ERROR_ACCESS_DENIED (0x887A002B). The working build never set it.
            nd.Flags = D3D12_RESOURCE_FLAG_NONE;
            ID3D12Resource* tex = nullptr;
            if (FAILED(g_game_device->CreateCommittedResource(&hp,
                    D3D12_HEAP_FLAG_NONE, &nd, D3D12_RESOURCE_STATE_COMMON,
                    nullptr, IID_PPV_ARGS(&tex))) || !tex) {
                InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(
                    &CyberpunkVR_DebugStableSkips));
                return;
            }
            tex->SetName(L"CyberpunkVR_VrcamStable");
            g_stable_tex = tex;
            g_stable_desc = d;
            log("[stable] committed vrcam snapshot=%p %llux%u fmt=%u", tex,
                (unsigned long long)d.Width, d.Height, (unsigned)d.Format);
        }
        stable = g_stable_tex;
    }
    // src: current state -> COPY_SOURCE (skip if already there); stable: COMMON ->
    // COPY_DEST; copy; restore BOTH so the engine's own state tracking stays intact.
    D3D12_RESOURCE_BARRIER b[2]{};
    UINT nb = 0;
    if (src_state != (uint32_t)D3D12_RESOURCE_STATE_COPY_SOURCE) {
        b[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[nb].Transition.pResource = src;
        b[nb].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[nb].Transition.StateBefore = (D3D12_RESOURCE_STATES)src_state;
        b[nb].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        ++nb;
    }
    b[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[nb].Transition.pResource = stable;
    b[nb].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[nb].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b[nb].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    ++nb;
    e->barrier_call(list, nb, b);
    e->copyres(list, stable, src);
    nb = 0;
    if (src_state != (uint32_t)D3D12_RESOURCE_STATE_COPY_SOURCE) {
        b[nb].Transition.pResource = src;
        b[nb].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b[nb].Transition.StateAfter  = (D3D12_RESOURCE_STATES)src_state;
        ++nb;
    }
    b[nb].Transition.pResource = stable;
    b[nb].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b[nb].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    ++nb;
    e->barrier_call(list, nb, b);
    g_stable_fresh.store(true, std::memory_order_release);
    g_stable_tick.store(GetTickCount64(), std::memory_order_release);
    CyberpunkVR_DebugStableSrcState = src_state;
    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(
        &CyberpunkVR_DebugStableCopies));
}

// ---- VRCAM eye source for the OpenXR stereo submit ----------------------------------
// Hands the submit path the VRCAM final colour for eye 1. This is the SAME resource the
// desktop mirror reads: written inside the ENGINE'S OWN command list at the end of the
// vrcam RenderFinal2D node, so by the time Present runs the copy is already ordered
// behind the final write and the resource rests in COMMON -- no barrier or fence wait
// is needed on the caller's side, and nothing can alias it (it is committed, not a
// transient from the frame-graph heap).
//
// Format note for the submit path: this carries the RenderFinal2D output format, which
// is R11G11B10_FLOAT holding LINEAR values -- NOT the sRGB-encoded R8G8B8A8_UNORM bytes
// the MAIN backbuffer holds. Eye 1 therefore needs the encode pass, not a raw copy.
// Returns null until the VRCAM node has produced its first frame (component disabled,
// or the first frames after load).
extern "C" __declspec(dllexport) ID3D12Resource* CyberpunkVR_GetVrcamEyeTexture() {
    if (CyberpunkVR_VrcamOwnTarget) {
        std::lock_guard<std::mutex> lk(g_own_target_mtx);
        if (g_own_target) return g_own_target;
    }
    if (!g_stable_fresh.load(std::memory_order_acquire)) return nullptr;
    std::lock_guard<std::mutex> lk(g_stable_mtx);
    return g_stable_tex;
}

// Same resource, but null once the second view has gone quiet -- the form the OpenXR submit
// wants. Existence is not enough there: handing eye 1 a snapshot the second view stopped
// updating pins one eye to a still image while the other keeps moving, which reads far worse
// than dropping to mono. Age also drives the overlay's status line.
extern "C" __declspec(dllexport) ID3D12Resource* CyberpunkVR_GetVrcamEyeTextureFresh() {
    const uint64_t last = g_stable_tick.load(std::memory_order_acquire);
    if (!last) { CyberpunkVR_DebugVrcamEyeAgeMs = 0xFFFFFFFFu; return nullptr; }
    const uint64_t age = GetTickCount64() - last;
    CyberpunkVR_DebugVrcamEyeAgeMs =
        age > 0xFFFFFFFEull ? 0xFFFFFFFEu : static_cast<uint32_t>(age);
    if (age > CyberpunkVR_StereoEyeMaxAgeMs) {
        // THIS RETURN IS THE MONO. Say why, once, with the tally that separates the four ways the
        // snapshot can stop -- and separates all of them from "the second view stopped rendering",
        // which is a different failure with the same symptom.
        //
        //   nodeHits   the vrcam CopyToTexture node still runs      (0 => the node is gone)
        //   noRtv      it ran but bound no output we recognise      (a rebuilt graph's new target)
        //   noList     output found, no hooked command list         (invisible to the publish
        //                                                            watchdog -- it needs neither)
        //   copies     the snapshot was attempted                   (climbing => look at skips)
        //   skips      attempted and refused inside the copy
        // ANSWERED, and now one level deeper. Measured: nodeHits climbs at frame rate while noRtv
        // climbs at exactly the same rate and copies stands still -- the node runs every frame and
        // binds an output that mirror_find_bound_rtv does not recognise. So the break is the RTV
        // CANDIDATE TABLE, and there are only two ways it can fail: the new target's RTV was never
        // registered (the gate in mirror_register_rtv refused it), or it was registered and then
        // EVICTED, the 512-slot ring having wrapped -- which is the third time an array in this
        // file would have quietly filled up. regs/evicts/live separate those two outright.
        static uint64_t s_lastSaidMs = 0;
        const uint64_t now = GetTickCount64();
        if (now - s_lastSaidMs > 5000) {
            s_lastSaidMs = now;
            log("[stereo-eye] stale by %llu ms (limit %u) -> submitting MONO. "
                "nodeHits=%llu noRtv=%llu noList=%llu copies=%llu skips=%llu attempted=%llu | "
                "rtv table: live=%u/%u regs=%llu evicts=%llu",
                (unsigned long long)age, CyberpunkVR_StereoEyeMaxAgeMs,
                (unsigned long long)g_eye_node_hits.load(std::memory_order_relaxed),
                (unsigned long long)g_eye_no_rtv.load(std::memory_order_relaxed),
                (unsigned long long)g_eye_no_list.load(std::memory_order_relaxed),
                (unsigned long long)CyberpunkVR_DebugStableCopies,
                (unsigned long long)CyberpunkVR_DebugStableSkips,
                (unsigned long long)g_eye_copy_calls.load(std::memory_order_relaxed),
                g_mirror_rtv_candidate_count.load(std::memory_order_relaxed),
                (unsigned)g_mirror_rtv_candidates.size(),
                (unsigned long long)CyberpunkVR_DebugMirrorRtvRegs,
                (unsigned long long)CyberpunkVR_DebugMirrorRtvEvicts);
            // And show the actual binds. [rtvpick] burned its budget down during startup, when
            // everything worked; re-arm a short burst here so the next few frames print the
            // handles the node is binding NOW and what, if anything, they resolve to.
            if (CyberpunkVR_DebugRtvPickLog <= 0) CyberpunkVR_DebugRtvPickLog = 6;
        }
        return nullptr;
    }
    return CyberpunkVR_GetVrcamEyeTexture();
}

// Append (into the game's OWN list) a copy dtex -> g_d12_mtex. Called from the
// barrier hook at the dtex's RENDER_TARGET->'after' transition, so dtex holds the
// freshly rendered frame and is in 'after'; bracket the copy with matching states.
static void d12_append_mirror_copy(const CommandListVtableHook* e,
        ID3D12GraphicsCommandList* list, ID3D12Resource* dtex,
        D3D12_RESOURCE_STATES after) {
    if (!e || !e->copyres || !e->barrier_call || !dtex || !g_d12_mtex) return;
    D3D12_RESOURCE_BARRIER b[2]{};
    b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource = dtex;
    b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition.StateBefore = after;
    b[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Transition.pResource = g_d12_mtex;
    b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    e->barrier_call(list, 2, b);
    e->copyres(list, g_d12_mtex, dtex);
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[0].Transition.StateAfter  = after;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    e->barrier_call(list, 2, b);
    g_mirror_pending_list.store(list, std::memory_order_release);
}

// One tiny copy submit on the GAME queue: dtex (rests permanently in RENDER_TARGET)
// -> g_d12_mtex. Called once per vrcam frame from ExecuteCommandLists right after
// the game submits the vrcam blit list, so on the queue timeline it runs after the
// blit and reads the freshly rendered frame. A 4-slot allocator ring is reused
// without stalling the game: if a slot's copy is still in flight, skip this mirror
// frame instead of waiting. Uses raw vtable barrier/copy to avoid re-entering the
// hooked ResourceBarrier, and g_orig_ExecuteCommandLists to bypass our own hook.
static ID3D12CommandAllocator*    g_d12_copy_alloc[4] = {};
static ID3D12GraphicsCommandList* g_d12_copy_list = nullptr;
static uint64_t                   g_d12_copy_slot_fence[4] = {};
static uint32_t                   g_d12_copy_frame = 0;
static std::mutex                 g_d12_copy_mtx;

// --- luma oscilloscope helpers (see exports block for the rationale) ---
static float luma_dec_f11(uint32_t v) {
    const uint32_t e = (v >> 6) & 0x1F, m = v & 0x3F;
    if (e == 0)  return m * (1.0f / 64.0f) * 0.00006103515625f;   // 2^-14
    if (e == 31) return 0.0f;                                     // inf/nan -> ignore
    return (1.0f + m / 64.0f) * exp2f((int)e - 15);
}
static float luma_dec_f10(uint32_t v) {
    const uint32_t e = (v >> 5) & 0x1F, m = v & 0x1F;
    if (e == 0)  return m * (1.0f / 32.0f) * 0.00006103515625f;
    if (e == 31) return 0.0f;
    return (1.0f + m / 32.0f) * exp2f((int)e - 15);
}
// ---- one-shot dump of the outline layer ------------------------------------------------------
// The second eye shows the layer as it is: silhouette fill AND outline. MAIN shows only the
// outline, because its chain (PS587 -> PS1047 -> PS1290 -> the PS1216 composite) derives edges
// from it, and that chain does not run for the second view. Before reproducing anything, look at
// what actually distinguishes an outline texel from a fill texel in this layer -- guessing that
// from the picture is how the previous three rounds went wrong.
// Write 1 to arm; the file lands next to the exe and the knob resets itself.
extern "C" __declspec(dllexport) int32_t CyberpunkVR_VisionDump = 1;
static ID3D12Resource* g_visdump_rb = nullptr;
static void*    g_visdump_map = nullptr;
static uint32_t g_visdump_w = 0, g_visdump_h = 0, g_visdump_pitch = 0;
static int      g_visdump_slot = -1;

static void vision_dump_write() {
    if (!g_visdump_map || !g_visdump_w || !g_visdump_h) return;
    char path[MAX_PATH]{};
    if (!GetModuleFileNameA(nullptr, path, MAX_PATH)) return;
    char* slash = strrchr(path, '\\');
    if (!slash) return;
    *(slash + 1) = '\0';
    strcat_s(path, "cyberpunkvr_vision.raw");
    HANDLE f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    // Rows written TIGHT (no 256-byte padding), so the file is a plain w*h RGBA8 raster.
    const uint8_t* base = static_cast<const uint8_t*>(g_visdump_map);
    const uint32_t row = g_visdump_w * 4;
    DWORD wrote = 0;
    for (uint32_t y = 0; y < g_visdump_h; ++y)
        WriteFile(f, base + static_cast<size_t>(y) * g_visdump_pitch, row, &wrote, nullptr);
    CloseHandle(f);
    log("[vision] dumped %ux%u RGBA8 -> %s", g_visdump_w, g_visdump_h, path);
}

static bool luma_probe_ensure(uint32_t idx) {
    if (g_luma_rb[idx]) return g_luma_map[idx] != nullptr;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 2048;                    // 8 rows x 256B pitch
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb = nullptr;
    if (FAILED(g_game_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb))) || !rb)
        return false;
    void* p = nullptr;
    if (FAILED(rb->Map(0, nullptr, &p)) || !p) { rb->Release(); return false; }
    g_luma_rb[idx] = rb;
    g_luma_map[idx] = static_cast<uint8_t*>(p);
    return true;
}
static double luma_probe_collect(uint32_t idx) {
    static double   s_sum[2] = {};
    static uint32_t s_n[2] = {};
    static double   s_dsum = 0.0;
    static uint32_t s_dn = 0;
    static double   s_last = -1.0;
    static uint32_t s_last_fr = 0xFFFFFFFFu;
    double L = 0.0;
    for (int y = 0; y < 8; ++y) {
        const uint32_t* row =
            reinterpret_cast<const uint32_t*>(g_luma_map[idx] + y * 256);
        for (int x = 0; x < 8; ++x) {
            const uint32_t v = row[x];
            L += 0.2126f * luma_dec_f11(v & 0x7FF)
               + 0.7152f * luma_dec_f11((v >> 11) & 0x7FF)
               + 0.0722f * luma_dec_f10((v >> 22) & 0x3FF);
        }
    }
    L /= 64.0;
    if (CyberpunkVR_LumaWave > 0) {
        --CyberpunkVR_LumaWave;            // benign race; diagnostic only
        log("[lumaw] fr=%u L=%.4f", g_luma_frame[idx], L);
    }
    const uint32_t p = g_luma_parity[idx] & 1u;
    s_sum[p] += L;
    ++s_n[p];
    // SAFE correlation: bucket L by the fin natural index used THAT frame (carried
    // through the readback ring, no cross-thread latency). Identifies which physical
    // index = dark(correct)/bright(wrong) AND whether the correct one is a stable
    // recurring (persistent) value -> tells us exactly what to pin, crash-free.
    {
        struct FinBucket { uint32_t idx; double sum; uint32_t n; };
        static FinBucket s_fb[12] = {};
        static uint32_t  s_fb_frames = 0;
        const uint32_t fi = g_luma_finidx[idx];
        for (int k = 0; k < 12; ++k) {
            if (s_fb[k].n == 0 || s_fb[k].idx == fi) {
                s_fb[k].idx = fi; s_fb[k].sum += L; ++s_fb[k].n; break;
            }
        }
        if (++s_fb_frames >= 240) {
            for (int k = 0; k < 12; ++k) {
                if (!s_fb[k].n) continue;
                log("[fincorr] finidx=%u meanL=%.4f n=%u",
                    s_fb[k].idx, s_fb[k].sum / s_fb[k].n, s_fb[k].n);
            }
            memset(s_fb, 0, sizeof(s_fb));
            s_fb_frames = 0;
        }
    }

    if (s_last >= 0.0 && g_luma_frame[idx] == s_last_fr + 1) {
        s_dsum += (L > s_last) ? (L - s_last) : (s_last - L);
        ++s_dn;
    }
    s_last = L;
    s_last_fr = g_luma_frame[idx];
    if (s_n[0] + s_n[1] >= 120) {
        const double e = s_n[0] ? s_sum[0] / s_n[0] : 0.0;
        const double o = s_n[1] ? s_sum[1] / s_n[1] : 0.0;
        const double dm = s_dn ? s_dsum / s_dn : 0.0;
        CyberpunkVR_DebugLumaEvenMilli  = (uint32_t)(e * 1000.0 + 0.5);
        CyberpunkVR_DebugLumaOddMilli   = (uint32_t)(o * 1000.0 + 0.5);
        CyberpunkVR_DebugLumaDeltaMilli = (uint32_t)(dm * 1000.0 + 0.5);
        log("[luma] n=%u/%u even=%.4f odd=%.4f dmean=%.4f(n=%u)",
            s_n[0], s_n[1], e, o, dm, s_dn);
        s_sum[0] = s_sum[1] = 0.0;
        s_n[0] = s_n[1] = 0;
        s_dsum = 0.0;
        s_dn = 0;
    }
    return L;
}

static bool cb_probe_ensure(uint32_t idx) {
    if (g_cb_rb[idx]) return g_cb_map[idx] != nullptr;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 1024;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb = nullptr;
    if (FAILED(g_game_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb))) || !rb)
        return false;
    void* p = nullptr;
    if (FAILED(rb->Map(0, nullptr, &p)) || !p) { rb->Release(); return false; }
    g_cb_rb[idx] = rb;
    g_cb_map[idx] = static_cast<uint8_t*>(p);
    return true;
}
// Auto-detect the raced CB dword: track per-dword value sets; a candidate holds
// EXACTLY two distinct values over the window and flips between them in sync with
// the bright/normal luma state. Scene-driven fields (matrices, time) take >2 values
// and disqualify themselves.
static void cb_probe_collect(uint32_t idx, double L) {
    // 232 dwords: 0..211 = tonemap CB, 214..220 = vrcam exposure accumulator (28B @
    // offset 856), 224..230 = main's (@896). 212/213/221..223/231 = padding (stale).
    struct CbDw { uint32_t v0, v1; uint8_t nv, last; uint32_t flips, agree, corr_n; };
    static CbDw     s_cb[232] = {};
    static uint32_t s_frames = 0;
    static double   s_lmin = 1e9, s_lmax = -1e9;
    if (g_cb_reset_pending.exchange(false, std::memory_order_acq_rel)) {
        memset(s_cb, 0, sizeof(s_cb));      // CB resource changed: restart window
        s_frames = 0;
        s_lmin = 1e9;
        s_lmax = -1e9;
    }
    if (L < s_lmin) s_lmin = L;
    if (L > s_lmax) s_lmax = L;
    const bool flap_active = (s_lmax - s_lmin) > 0.02;
    const bool bright = flap_active && (L > (s_lmin + s_lmax) * 0.5);
    const uint32_t* dw = reinterpret_cast<const uint32_t*>(g_cb_map[idx]);
    for (int i = 0; i < 232; ++i) {
        CbDw& c = s_cb[i];
        const uint32_t v = dw[i];
        uint8_t state;
        if (c.nv == 0)            { c.v0 = v; c.nv = 1; state = 0; }
        else if (v == c.v0)       { state = 0; }
        else if (c.nv == 1)       { c.v1 = v; c.nv = 2; state = 1; }
        else if (v == c.v1)       { state = 1; }
        else                      { c.nv = 3; continue; }      // >2 values: disqualified
        if (c.nv == 2) {
            if (state != c.last) ++c.flips;
            if (flap_active) {
                if ((state != 0) == bright) ++c.agree;         // orientation A
                ++c.corr_n;
            }
        }
        c.last = state;
    }
    if (++s_frames >= 300) {
        const float* xv = reinterpret_cast<const float*>(g_cb_map[idx] + 856);
        const float* xm = reinterpret_cast<const float*>(g_cb_map[idx] + 896);
        log("[expo] v=%.5g %.5g %.5g %.5g %.5g %.5g %.5g | m=%.5g %.5g %.5g %.5g %.5g %.5g %.5g",
            xv[0], xv[1], xv[2], xv[3], xv[4], xv[5], xv[6],
            xm[0], xm[1], xm[2], xm[3], xm[4], xm[5], xm[6]);
        int b1 = -1, b2 = -1;
        for (int i = 0; i < 232; ++i) {
            const CbDw& c = s_cb[i];
            if (c.nv != 2 || c.flips < 8) continue;
            if (b1 < 0 || c.flips > s_cb[b1].flips) { b2 = b1; b1 = i; }
            else if (b2 < 0 || c.flips > s_cb[b2].flips) { b2 = i; }
        }
        for (int k = 0; k < 2; ++k) {
            const int i = (k == 0) ? b1 : b2;
            if (i < 0) continue;
            const CbDw& c = s_cb[i];
            const uint32_t hi = (c.corr_n && c.agree > c.corr_n / 2)
                ? c.agree : c.corr_n - c.agree;
            const bool hit = (c.corr_n >= 30 && hi * 10 >= c.corr_n * 9);
            float f0, f1;
            memcpy(&f0, &c.v0, 4);
            memcpy(&f1, &c.v1, 4);
            log("[cbflap] %s off=0x%03X v0=%08X(%.4f) v1=%08X(%.4f) flips=%u sync=%u/%u",
                hit ? "HIT " : "cand", i * 4, c.v0, f0, c.v1, f1, c.flips,
                hi, c.corr_n);
        }
        if (b1 < 0)
            log("[cbflap] window done: no 2-valued dword (flap %s)",
                flap_active ? "ACTIVE" : "inactive");
        memset(s_cb, 0, sizeof(s_cb));
        s_frames = 0;
        s_lmin = 1e9;
        s_lmax = -1e9;
    }
}

static bool ti_probe_ensure(uint32_t idx) {
    if (g_ti_rb[idx]) return g_ti_map[idx] != nullptr;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 65536;                       // 24 x 2KB sections + slack
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb = nullptr;
    if (FAILED(g_game_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb))) || !rb)
        return false;
    void* p = nullptr;
    if (FAILED(rb->Map(0, nullptr, &p)) || !p) { rb->Release(); return false; }
    g_ti_rb[idx] = rb;
    g_ti_map[idx] = static_cast<uint8_t*>(p);
    return true;
}
static float half2f(uint16_t h) {
    const uint32_t e = (h >> 10) & 0x1F, m = h & 0x3FF;
    float f;
    if (e == 0)       f = m * (1.0f / 1024.0f) * 0.00006103515625f;
    else if (e == 31) f = 0.0f;
    else              f = (1.0f + m / 1024.0f) * exp2f((int)e - 15);
    return (h & 0x8000) ? -f : f;
}
// Green-channel mean of an 8x8 block (flap detection needs any monotonic channel).
static double ti_block_green(const uint8_t* base, uint32_t fmt) {
    double s = 0.0;
    for (int y = 0; y < 8; ++y) {
        const uint8_t* row = base + y * 256;
        for (int x = 0; x < 8; ++x) {
            switch (fmt) {
            case DXGI_FORMAT_R11G11B10_FLOAT:
                s += luma_dec_f11((reinterpret_cast<const uint32_t*>(row)[x] >> 11)
                                  & 0x7FF);
                break;
            case DXGI_FORMAT_R10G10B10A2_UNORM:
                s += ((reinterpret_cast<const uint32_t*>(row)[x] >> 10) & 0x3FF)
                     * (1.0 / 1023.0);
                break;
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
                s += half2f(reinterpret_cast<const uint16_t*>(row)[x * 4 + 1]);
                break;
            case DXGI_FORMAT_R32_FLOAT:
                s += reinterpret_cast<const float*>(row)[x];
                break;
            default:    // 8-bit RGBA variants: byte 1 = G
                s += row[x * 4 + 1] * (1.0 / 255.0);
                break;
            }
        }
    }
    return s / 64.0;
}
static void ti_probe_collect(uint32_t idx, double L) {
    struct Stat {
        ID3D12Resource* res; uint32_t fmt; uint32_t tag;
        double sum[2]; uint32_t n[2];
    };
    static Stat     s_st[32] = {};
    static uint32_t s_frames = 0;
    static double   s_lmin = 1e9, s_lmax = -1e9;
    if (L < s_lmin) s_lmin = L;
    if (L > s_lmax) s_lmax = L;
    const bool flap_active = (s_lmax - s_lmin) > 0.02;
    const int bright = (flap_active && L > (s_lmin + s_lmax) * 0.5) ? 1 : 0;
    if (flap_active) {
        for (uint32_t i = 0; i < g_ti_count[idx] && i < 24; ++i) {
            ID3D12Resource* res = g_ti_src[idx][i];
            if (!res) continue;
            const double g =
                ti_block_green(g_ti_map[idx] + i * 2048, g_ti_fmt[idx][i]);
            for (int k = 0; k < 32; ++k) {
                if (s_st[k].res == res || !s_st[k].res) {
                    s_st[k].res = res;
                    s_st[k].fmt = g_ti_fmt[idx][i];
                    s_st[k].tag = g_ti_tag[idx][i];
                    s_st[k].sum[bright] += g;
                    ++s_st[k].n[bright];
                    break;
                }
            }
        }
    }
    if (++s_frames >= 300) {
        for (int k = 0; k < 32; ++k) {
            const Stat& t = s_st[k];
            if (!t.res || (t.n[0] + t.n[1]) < 30) continue;
            const double mn = t.n[0] ? t.sum[0] / t.n[0] : 0.0;
            const double mb = t.n[1] ? t.sum[1] / t.n[1] : 0.0;
            log("[chain] res=%p fmt=%u node=0x%X normal=%.4f(n=%u) bright=%.4f(n=%u) split=%+.4f",
                t.res, t.fmt, t.tag, mn, t.n[0], mb, t.n[1], mb - mn);
        }
        memset(s_st, 0, sizeof(s_st));
        s_frames = 0;
        s_lmin = 1e9;
        s_lmax = -1e9;
    }
}

static void d12_submit_mirror_copy(ID3D12CommandQueue* queue) {
    if (!CyberpunkVR_MirrorOutput || !queue || !g_game_device) return;
    const bool use_own = (CyberpunkVR_VrcamOwnTarget && g_own_target);
    // Prefer the stable committed snapshot (filled inline in the valid window, never
    // aliased, known COMMON state) over the transient whose heap main re-uses.
    ID3D12Resource* stable_src = nullptr;
    if (!use_own && CyberpunkVR_StableCopy &&
            g_stable_fresh.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lk(g_stable_mtx);
        stable_src = g_stable_tex;
    }
    const bool use_stable = (stable_src != nullptr);
    ID3D12Resource* dtex = use_own ? g_own_target
        : use_stable ? stable_src
        : g_captured_vrcam_res.load(std::memory_order_acquire);
    if (!dtex) return;
    std::unique_lock<std::mutex> lk(g_d12_copy_mtx, std::try_to_lock);
    if (!lk.owns_lock()) return;                          // single-flight; never stall
    D3D12_RESOURCE_DESC d{};
    if (!mirror_get_resource_desc(dtex, &d)) return;
    if (!d12_mirror_ensure(d)) return;                    // create mtex + fence once
    if (!g_d12_copy_list) {
        for (int i = 0; i < 4; ++i)
            if (FAILED(g_game_device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_d12_copy_alloc[i]))))
                return;
        if (FAILED(g_game_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                g_d12_copy_alloc[0], nullptr, IID_PPV_ARGS(&g_d12_copy_list))))
            return;
        g_d12_copy_list->Close();
    }
    const uint32_t idx = g_d12_copy_frame & 3u;
    if (g_d12_fence->GetCompletedValue() < g_d12_copy_slot_fence[idx]) return;  // in flight
    // Slot idx's previous submission is GPU-complete -> its readbacks are coherent.
    if (CyberpunkVR_LumaProbe && g_luma_valid[idx] && g_luma_map[idx]) {
        const double L = luma_probe_collect(idx);
        g_luma_valid[idx] = false;
        if (g_cb_valid[idx] && g_cb_map[idx]) cb_probe_collect(idx, L);
        if (g_ti_valid[idx] && g_ti_map[idx]) ti_probe_collect(idx, L);
    }
    g_cb_valid[idx] = false;
    g_ti_valid[idx] = false;
    if (g_visdump_slot == static_cast<int>(idx) && CyberpunkVR_VisionDump == 2) {
        vision_dump_write();
        g_visdump_slot = -1;
        CyberpunkVR_VisionDump = 0;
    }
    if (FAILED(g_d12_copy_alloc[idx]->Reset())) return;
    if (FAILED(g_d12_copy_list->Reset(g_d12_copy_alloc[idx], nullptr))) return;
    const CommandListVtableHook* e = command_list_hook_entry(g_d12_copy_list);
    if (!e || !e->barrier_call || !e->copyres) { g_d12_copy_list->Close(); return; }
    // Use the state hk_ResourceBarrier actually tracked for THIS captured resource (its
    // real resting state at copy time), NOT a fixed guess. A wrong StateBefore makes the
    // barrier a hazard and the copy reads stale/aliased heap memory (main's content) ->
    // bright/dark alternation. Fall back to the tunable only if nothing tracked yet.
    // Our own committed target always rests in RENDER_TARGET (engine renders into it; the
    // copy transitions RT->COPY_SOURCE->RT). For the fallback transient, use the actually-
    // tracked state (not a fixed guess).
    D3D12_RESOURCE_STATES copy_src_state = use_own
        ? D3D12_RESOURCE_STATE_RENDER_TARGET
        : use_stable ? D3D12_RESOURCE_STATE_COMMON   // stable rests in COMMON
        : (D3D12_RESOURCE_STATES)CyberpunkVR_MirrorCopyState;
    if (!use_own && !use_stable && CyberpunkVR_MirrorTrackState) {
        const uint32_t tracked = CyberpunkVR_DebugMirrorSrcState;
        if (tracked != 0) copy_src_state = (D3D12_RESOURCE_STATES)tracked;
    }
    // ---- HUD on the mirror -----------------------------------------------------------------
    // The second-eye composite is otherwise only visible inside the headset, which makes it
    // untestable at a desk. Running it here shows exactly what eye 1 gets, on OUR OWN list.
    // When it runs it REPLACES the plain copy: it reads the same source and writes the composite
    // straight into the mirror texture, so there is no copy to pay for as well.
    ID3D12Resource* hudTex  = g_d12_mtex_is_rt ? CyberpunkVR_GetHudTexture() : nullptr;
    ID3D12Resource* hudBlur = hudTex ? CyberpunkVR_GetHudBlurTexture() : nullptr;
    ID3D12Resource* hudExpo = hudTex ? CyberpunkVR_GetHudExposureBuffer() : nullptr;
    ID3D12Resource* frameCb = hudTex ? CyberpunkVR_GetFrameConstantBuffer() : nullptr;
    // Optional: when it is missing the shader's own validity check rejects whatever we bind in
    // its place and falls back to the captured constants. Requiring it here is what made the HUD
    // vanish entirely -- a guard that contradicted the fallback the shader already had.
    ID3D12Resource* hudCb   = hudTex ? CyberpunkVR_GetHudConstantBuffer() : nullptr;
    CyberpunkVR_NoteHudCompositeInputs(hudTex, hudBlur, hudExpo, frameCb, hudCb);
    if (!hudCb) hudCb = frameCb;
    bool hud_composited = false;
    if (hudTex && hudBlur && hudExpo && frameCb && hudCb &&
        g_hud_mirror_blit.EnsureInitialized(g_game_device, g_d12_fmt, g_d12_w, g_d12_h)) {
        D3D12_RESOURCE_BARRIER mb[2]{};
        for (int i = 0; i < 2; ++i) {
            mb[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            mb[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        mb[0].Transition.pResource = g_d12_mtex;
        mb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        mb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        // The scene has to be readable by the pixel shader; it normally rests in COMMON, which
        // promotes implicitly, so only a non-COMMON source needs an explicit transition.
        UINT nb = 1;
        if (copy_src_state != D3D12_RESOURCE_STATE_COMMON) {
            mb[1].Transition.pResource = dtex;
            mb[1].Transition.StateBefore = copy_src_state;
            mb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            nb = 2;
        }
        e->barrier_call(g_d12_copy_list, nb, mb);
        hud_composited = g_hud_mirror_blit.RecordHudComposite(
            g_d12_copy_list, dtex, hudTex, hudBlur, hudExpo, frameCb, hudCb, g_d12_mtex,
            hud_composite_params());
        // The scanner's outline, on top, while the mirror texture is still a render target.
        // It belongs here and not only in the headset path for a plain reason: this window IS
        // the desk-side view of eye 1, and the headset path only runs inside an XR session --
        // with no session its counters stay at zero and nothing composites anywhere, which is
        // exactly what "the outline did not appear" turned out to mean.
        if (hud_composited && CyberpunkVR_VisionToSecondEye) {
            if (ID3D12Resource* vis = CyberpunkVR_GetVisionTexture()) {
                if (g_hud_mirror_blit.RecordOverlay(g_d12_copy_list, vis, g_d12_mtex,
                                                    CyberpunkVR_VisionDebug,
                                                    CyberpunkVR_VisionFit != 0,
                                                    CyberpunkVR_VisionOffX,
                                                    CyberpunkVR_VisionOffY))
                    ++CyberpunkVR_DebugVisionOverlays;
            }
        }
        // The barrel dot, at the same NDC the overlay drew it at on the backbuffer. The mirror
        // window IS the desk-side view of eye 1, so without this the dot is invisible whenever
        // there is no headset session -- which is most of the time while working on it.
        if (hud_composited && CyberpunkVR_BarrelDotSecondEye && CyberpunkVR_BarrelDotTick &&
            GetTickCount64() - CyberpunkVR_BarrelDotTick < 250) {
            if (g_hud_mirror_blit.RecordDot(g_d12_copy_list, g_d12_mtex,
                                            CyberpunkVR_BarrelDotNdcX2,
                                            CyberpunkVR_BarrelDotNdcY,
                                            CyberpunkVR_BarrelDotRadiusPx,
                                            1.0f, 0.045f, 0.045f, 1.0f))
                ++CyberpunkVR_DebugBarrelDotDraws;
        }
        // Armed dump of the same layer, on the same list and the same fence as everything else
        // here. The snapshot rests in COMMON, which promotes implicitly for a copy source, so
        // this asserts nothing about anyone else's state.
        if (CyberpunkVR_VisionDump == 1) {
            ID3D12Resource* vis = CyberpunkVR_GetVisionTexture();
            D3D12_RESOURCE_DESC vdd{};
            if (vis && mirror_get_resource_desc(vis, &vdd)) {
                const uint32_t w = static_cast<uint32_t>(vdd.Width), h = vdd.Height;
                const uint32_t pitch = (w * 4 + 255u) & ~255u;
                const uint64_t need = static_cast<uint64_t>(pitch) * h;
                if (g_visdump_rb && (g_visdump_w != w || g_visdump_h != h)) {
                    g_visdump_rb->Unmap(0, nullptr);
                    g_visdump_rb->Release();
                    g_visdump_rb = nullptr; g_visdump_map = nullptr;
                }
                if (!g_visdump_rb) {
                    D3D12_HEAP_PROPERTIES rp{}; rp.Type = D3D12_HEAP_TYPE_READBACK;
                    rp.CreationNodeMask = rp.VisibleNodeMask = 1;
                    D3D12_RESOURCE_DESC bd{};
                    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                    bd.Width = need; bd.Height = 1; bd.DepthOrArraySize = 1;
                    bd.MipLevels = 1; bd.Format = DXGI_FORMAT_UNKNOWN;
                    bd.SampleDesc.Count = 1;
                    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                    if (SUCCEEDED(g_game_device->CreateCommittedResource(
                            &rp, D3D12_HEAP_FLAG_NONE, &bd,
                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                            IID_PPV_ARGS(&g_visdump_rb))) && g_visdump_rb) {
                        if (FAILED(g_visdump_rb->Map(0, nullptr, &g_visdump_map)))
                            g_visdump_map = nullptr;
                        g_visdump_w = w; g_visdump_h = h; g_visdump_pitch = pitch;
                    }
                }
                if (g_visdump_rb && g_visdump_map) {
                    D3D12_TEXTURE_COPY_LOCATION vs{}, vdst{};
                    vs.pResource = vis;
                    vs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    vs.SubresourceIndex = 0;
                    vdst.pResource = g_visdump_rb;
                    vdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    vdst.PlacedFootprint.Offset = 0;
                    vdst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    vdst.PlacedFootprint.Footprint.Width = w;
                    vdst.PlacedFootprint.Footprint.Height = h;
                    vdst.PlacedFootprint.Footprint.Depth = 1;
                    vdst.PlacedFootprint.Footprint.RowPitch = pitch;
                    g_d12_copy_list->CopyTextureRegion(&vdst, 0, 0, 0, &vs, nullptr);
                    g_visdump_slot = static_cast<int>(idx);
                    CyberpunkVR_VisionDump = 2;
                }
            } else {
                log("[vision] dump armed but no layer available -- scan something first");
                CyberpunkVR_VisionDump = 0;
            }
        }
        mb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        mb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
        if (nb == 2) {
            mb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            mb[1].Transition.StateAfter  = copy_src_state;
        }
        e->barrier_call(g_d12_copy_list, nb, mb);
    }
    if (hud_composited) g_mirror_pending_list.store(g_d12_copy_list, std::memory_order_release);
    else d12_append_mirror_copy(e, g_d12_copy_list, dtex, copy_src_state);
    // Luma probe: 8x8 center of the stable snapshot -> readback slot (same list, same
    // fence). Stable rests in COMMON between our operations; bracket accordingly.
    if (CyberpunkVR_LumaProbe && use_stable && luma_probe_ensure(idx)) {
        D3D12_RESOURCE_BARRIER lb{};
        lb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        lb.Transition.pResource = dtex;
        lb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        lb.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        lb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        e->barrier_call(g_d12_copy_list, 1, &lb);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = dtex;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = g_luma_rb[idx];
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format = d.Format;
        dst.PlacedFootprint.Footprint.Width = 8;
        dst.PlacedFootprint.Footprint.Height = 8;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = 256;
        const UINT cx = (UINT)(d.Width / 2), cy = (UINT)(d.Height / 2);
        D3D12_BOX box{ cx - 4, cy - 4, 0, cx + 4, cy + 4, 1 };
        g_d12_copy_list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);  // slot16 unhooked
        lb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        lb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
        e->barrier_call(g_d12_copy_list, 1, &lb);
        g_luma_parity[idx] = g_d12_copy_frame & 1u;
        g_luma_frame[idx]  = g_d12_copy_frame;
        g_luma_finidx[idx] = CyberpunkVR_DebugFinNatural;  // which fin version this frame
        g_luma_valid[idx]  = true;
    }
    // CB-flap probe: read the captured tonemap CB (848B) with the same fence. The CB
    // rests in VERTEX_AND_CONSTANT_BUFFER between frames (engine brackets its own
    // uploads the same way); queue order after this frame's submission = safe window.
    ID3D12Resource* cbres = g_cb_res.load(std::memory_order_acquire);
    if (cbres && g_cb_last_res.exchange(cbres, std::memory_order_acq_rel) != cbres) {
        g_cb_reset_pending.store(true, std::memory_order_release);
        // Graph rebuilt (CB resource changed): restart the chain-capture set so it
        // repopulates with live resources (old AddRef'd entries intentionally leak).
        g_tm_in_n.store(0, std::memory_order_release);
    }
    if (CyberpunkVR_LumaProbe && use_stable && cb_probe_ensure(idx)) {
        D3D12_RESOURCE_BARRIER cb{};
        cb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        cb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bool any = false;
        if (cbres) {
            cb.Transition.pResource = cbres;
            cb.Transition.StateBefore =
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
            cb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            e->barrier_call(g_d12_copy_list, 1, &cb);
            g_d12_copy_list->CopyBufferRegion(g_cb_rb[idx], 0, cbres,
                g_cb_off.load(std::memory_order_acquire), 848);
            cb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            cb.Transition.StateAfter =
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
            e->barrier_call(g_d12_copy_list, 1, &cb);
            any = true;
        }
        // Both views' adapted-exposure accumulators (28B; rest in PS|NPS after the
        // adaptation pass): vrcam -> offset 856, main -> offset 896.
        const D3D12_RESOURCE_STATES kExpoRest =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        ID3D12Resource* expo[2] = {
            g_expo_vrcam.load(std::memory_order_acquire),
            g_expo_main.load(std::memory_order_acquire) };
        for (int ei = 0; ei < 2; ++ei) {
            if (!expo[ei]) continue;
            cb.Transition.pResource = expo[ei];
            cb.Transition.StateBefore = kExpoRest;
            cb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            e->barrier_call(g_d12_copy_list, 1, &cb);
            g_d12_copy_list->CopyBufferRegion(g_cb_rb[idx],
                (ei == 0) ? 856 : 896, expo[ei], 0, 28);
            cb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            cb.Transition.StateAfter = kExpoRest;
            e->barrier_call(g_d12_copy_list, 1, &cb);
            any = true;
        }
        g_cb_valid[idx] = any;
    }
    // Stage sampling: 8x8 center of each captured texture (whole-chain set). State =
    // last tracked StateAfter, restored after the copy. TYPELESS mapped to concrete.
    const uint32_t tin_n = g_tm_in_n.load(std::memory_order_acquire);
    if (CyberpunkVR_LumaProbe && use_stable && tin_n && ti_probe_ensure(idx)) {
        uint32_t out_i = 0;
        for (uint32_t i = 0; i < tin_n && i < 24 && out_i < 24; ++i) {
            TmInCap& cap = g_tm_in[i];
            ID3D12Resource* res = cap.res.load(std::memory_order_relaxed);
            const uint32_t st = cap.state.load(std::memory_order_relaxed);
            if (!res) continue;
            D3D12_RESOURCE_DESC rd{};
            if (!mirror_get_resource_desc(res, &rd)) continue;
            uint32_t cf = (uint32_t)rd.Format;
            if (cf == 23) cf = 24;          // R10G10B10A2_TYPELESS -> UNORM
            else if (cf == 9) cf = 10;      // R16G16B16A16_TYPELESS -> FLOAT
            else if (cf == 27) cf = 28;     // R8G8B8A8_TYPELESS -> UNORM
            else if (cf == 39) cf = 41;     // R32_TYPELESS -> R32_FLOAT
            // WHITELIST: any other format (depth R24G8, BC, R16 variants...) makes
            // the recorded CopyTextureRegion invalid -> the deferred list's Close()
            // fails EVERY frame -> mirror+probes freeze (observed). Skip them.
            if (cf != 10 && cf != 24 && cf != 26 && cf != 28 && cf != 41 &&
                cf != 87) {
                continue;
            }
            D3D12_RESOURCE_BARRIER tb{};
            tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            tb.Transition.pResource = res;
            tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            tb.Transition.StateBefore = (D3D12_RESOURCE_STATES)st;
            tb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            const bool need =
                (tb.Transition.StateBefore != D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (need) e->barrier_call(g_d12_copy_list, 1, &tb);
            D3D12_TEXTURE_COPY_LOCATION tsrc{}, tdst{};
            tsrc.pResource = res;
            tsrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            tsrc.SubresourceIndex = 0;
            tdst.pResource = g_ti_rb[idx];
            tdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            tdst.PlacedFootprint.Offset = out_i * 2048;
            tdst.PlacedFootprint.Footprint.Format = (DXGI_FORMAT)cf;
            tdst.PlacedFootprint.Footprint.Width = 8;
            tdst.PlacedFootprint.Footprint.Height = 8;
            tdst.PlacedFootprint.Footprint.Depth = 1;
            tdst.PlacedFootprint.Footprint.RowPitch = 256;
            const UINT tcx = (UINT)(rd.Width / 2), tcy = (UINT)(rd.Height / 2);
            D3D12_BOX tbox{ tcx - 4, tcy - 4, 0, tcx + 4, tcy + 4, 1 };
            g_d12_copy_list->CopyTextureRegion(&tdst, 0, 0, 0, &tsrc, &tbox);
            if (need) {
                tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                tb.Transition.StateAfter = (D3D12_RESOURCE_STATES)st;
                e->barrier_call(g_d12_copy_list, 1, &tb);
            }
            g_ti_src[idx][out_i] = res;
            g_ti_fmt[idx][out_i] = cf;
            g_ti_tag[idx][out_i] = g_tm_in_rva[i].load(std::memory_order_relaxed);
            ++out_i;
        }
        g_ti_count[idx] = out_i;
        g_ti_valid[idx] = out_i != 0;
    }
    if (FAILED(g_d12_copy_list->Close())) {
        static std::atomic<uint64_t> s_close_fails{0};
        const uint64_t n = s_close_fails.fetch_add(1) + 1;
        if (n == 1 || (n % 512) == 0)
            log("[mirror] deferred list Close FAILED (n=%llu) -- invalid recorded "
                "command; probes+mirror stall until fixed", (unsigned long long)n);
        return;
    }
    ID3D12CommandList* ls[1] = { g_d12_copy_list };
    g_orig_ExecuteCommandLists(queue, 1, ls);             // bypass our own ECL hook
    const uint64_t v = g_d12_fence_next.fetch_add(1, std::memory_order_relaxed);
    if (SUCCEEDED(queue->Signal(g_d12_fence, v))) {
        g_d12_copy_slot_fence[idx] = v;
        g_d12_ready.store(v, std::memory_order_release);
        CyberpunkVR_DebugMirrorReadyFence = v;
    }
    ++g_d12_copy_frame;
    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(
        &CyberpunkVR_DebugMirrorPendingHits));
    bool ex = false;
    if (g_d12_present_started.compare_exchange_strong(ex, true))
        std::thread(d12_present_thread).detach();
}

static void STDMETHODCALLTYPE hk_ResourceBarrier(ID3D12GraphicsCommandList* self,
        UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
    const CommandListVtableHook* e = command_list_hook_entry(self);
    PFN_ResourceBarrier orig = e ? e->barrier_original : nullptr;
    if (orig) orig(self, count, barriers);
    // Gated on the same demand as the snapshot, NOT on the mirror window alone. This hook is
    // where t_mirror_src_state is refined (below): the OM bind seeds it as RENDER_TARGET and
    // every later transition of that target is tracked here, so the inline copy can name the
    // correct "before" state in its own barrier. Leaving it mirror-only meant the eye-capture
    // path would copy with a stale state -- a barrier lie, which the debug layer rejects and
    // the driver may answer with device removal.
    if (!stereo_eye_capture_wanted() || !barriers || !e) return;
    // ---- the HUD snapshot, taken once the glow mips exist ------------------------------------
    //
    // This used to fire at the OMSetRenderTargets that unbinds the HUD's mip-0 target, which is
    // BEFORE the engine builds mips 1..4. The composite samples exactly those mips for the glow,
    // so it was reading undefined memory: no quest/map/weapon highlight and thin text. The last
    // mip becomes shader-readable in one barrier, and at that moment ALL five subresources sit in
    // the same PIXEL|NON_PIXEL_SHADER_RESOURCE state -- the one point where a single
    // ALL_SUBRESOURCES transition is honest.
    if (CyberpunkVR_HudToSecondEye && g_hud_res) {
        ID3D12Resource* hud_ready = nullptr;
        const uint64_t tick = g_stable_tick.load(std::memory_order_acquire);
        const uint64_t now = GetTickCount64();
        const uint64_t used = g_hud_consumed_tick.load(std::memory_order_acquire);
        const bool live = tick && (now - tick) < 2000 &&
            (!g_hud_snap_fresh.load(std::memory_order_acquire) ||
             (used && (now - used) < 2000));
        const D3D12_RESOURCE_STATES kShaderRead =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        ID3D12Resource* blur_ready = nullptr;
        if (live) {
            const uint64_t hudW = g_hud_snap_desc.Width ? g_hud_snap_desc.Width : 0;
            for (UINT i = 0; i < count; ++i) {
                const D3D12_RESOURCE_BARRIER& b = barriers[i];
                if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
                if (!(b.Transition.StateAfter & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))
                    continue;
                if (b.Transition.pResource == g_hud_res) {
                    // The LAST mip: every earlier one was already released to the shader as the
                    // chain was walked, so this is the transition that completes it.
                    if (b.Transition.Subresource ==
                        g_hud_last_mip.load(std::memory_order_acquire)) {
                        hud_ready = g_hud_res;
                    }
                    continue;
                }
                // The blurred-HUD pyramid, on the release of ITS last mip (index 3), where all
                // four subresources are likewise uniform. The engine ping-pongs two of these and
                // reads the one released LAST, so simply letting the later copy win picks it.
                D3D12_RESOURCE_DESC bd{};
                if (b.Transition.Subresource == 3 && hudW &&
                    mirror_get_resource_desc(b.Transition.pResource, &bd) &&
                    hud_blur_signature(bd, hudW)) {
                    blur_ready = b.Transition.pResource;
                }
            }
        }
        // MAIN's finished frame and its scene, both released in the SAME barrier call that
        // retires the HUD. Their descs are not distinctive on their own (several textures share
        // them), but that co-occurrence is: it happens once per frame, at the end of the
        // composite, and nothing else releases the HUD.
        ID3D12Resource* main_out = nullptr;
        ID3D12Resource* main_scene = nullptr;
        D3D12_RESOURCE_STATES main_out_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        D3D12_RESOURCE_STATES main_scene_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        bool hud_retired = false;
        for (UINT i = 0; i < count; ++i) {
            const D3D12_RESOURCE_BARRIER& b = barriers[i];
            if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
                b.Transition.pResource == g_hud_res &&
                b.Transition.StateAfter == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
                hud_retired = true;
                break;
            }
        }
        if (hud_retired && live) {
            uint64_t w = 0, h = 0;
            {
                std::lock_guard<std::mutex> lk(g_hud_snap_mtx);
                w = g_hud_snap_desc.Width; h = g_hud_snap_desc.Height;
            }
            for (UINT i = 0; i < count && w; ++i) {
                const D3D12_RESOURCE_BARRIER& b = barriers[i];
                if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
                D3D12_RESOURCE_DESC rd{};
                if (!b.Transition.pResource ||
                    !mirror_get_resource_desc(b.Transition.pResource, &rd)) continue;
                if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                    rd.Width != w || rd.Height != h || rd.MipLevels != 1) continue;
                if (!g_hud_batch_listed) {
                    log("[hud] retire batch: res=%p %llux%u fmt=%u before=%u after=%u",
                        b.Transition.pResource, (unsigned long long)rd.Width, rd.Height,
                        (unsigned)rd.Format, (unsigned)b.Transition.StateBefore,
                        (unsigned)b.Transition.StateAfter);
                }
                // The composite's own output is an 8-bit colour target at output size; the scene
                // it read is the float one. Deliberately not keyed on the transition states --
                // guessing those is what missed it last time.
                if (rd.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                    rd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
                    rd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                    rd.Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
                    main_out = b.Transition.pResource;
                    main_out_state = b.Transition.StateAfter;
                } else if (rd.Format == DXGI_FORMAT_R11G11B10_FLOAT) {
                    main_scene = b.Transition.pResource;
                    main_scene_state = b.Transition.StateAfter;
                }
            }
        }

        if (hud_ready)  hud_snapshot_copy(self, hud_ready, kSnapHud, kShaderRead);
        if (blur_ready) hud_snapshot_copy(self, blur_ready, kSnapBlur, kShaderRead);
        // (MAIN's finished frame and its scene are no longer copied: the scene-swap they were
        //  for assumed the composite leaves the scene untouched, which it does not -- it applies
        //  aberration, vignette and grain to the scene itself, so out_main - S_main is far from
        //  zero away from the HUD and MAIN's picture bled over the whole eye.)
        (void)main_out; (void)main_scene; (void)main_out_state; (void)main_scene_state;
        if (hud_retired && !g_hud_batch_listed) {
            g_hud_batch_listed = true;
            log("[hud] retire batch resolved: out=%p scene=%p", main_out, main_scene);
        }
    }
    // Whole-chain capture: during ANY vrcam node, remember every large texture
    // transitioned to a read state (PS 0x80 / NON_PS 0x40), tagged with the node's
    // work RVA. Maps the entire per-frame stage chain in one run.
    if (t_vrcam_node_active) {
        for (UINT i = 0; i < count; ++i) {
            const D3D12_RESOURCE_BARRIER& b = barriers[i];
            if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
                !b.Transition.pResource ||
                !(b.Transition.StateAfter &
                  (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))) {
                continue;
            }
            D3D12_RESOURCE_DESC rd{};
            if (!mirror_get_resource_desc(b.Transition.pResource, &rd) ||
                rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                rd.Width < 1000) {
                continue;
            }
            const uint32_t rva = t_current_node_work
                ? (uint32_t)(t_current_node_work -
                             reinterpret_cast<uintptr_t>(g_exe_base))
                : 0;
            tm_set_push(b.Transition.pResource,
                (uint32_t)b.Transition.StateAfter, (uint32_t)rd.Format, rva);
        }
    }
    // Keep captured pointers' LAST StateAfter fresh (any list, any thread).
    {
        const uint32_t ntm = g_tm_in_n.load(std::memory_order_acquire);
        if (ntm) {
            for (UINT i = 0; i < count; ++i) {
                const D3D12_RESOURCE_BARRIER& b = barriers[i];
                if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
                    !b.Transition.pResource) {
                    continue;
                }
                for (uint32_t k = 0; k < ntm && k < 24; ++k) {
                    if (g_tm_in[k].res.load(std::memory_order_relaxed) ==
                            b.Transition.pResource) {
                        g_tm_in[k].state.store((uint32_t)b.Transition.StateAfter,
                                               std::memory_order_relaxed);
                    }
                }
            }
        }
    }
    // Adapted-exposure accumulator capture: BUFFER W=28 leaving UNORDERED_ACCESS for
    // PS|NPS = the adaptation dispatch just wrote it; attribute by recording view.
    for (UINT i = 0; i < count; ++i) {
        const D3D12_RESOURCE_BARRIER& b = barriers[i];
        if (CyberpunkVR_VisionSnap && b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV &&
            b.UAV.pResource) {
            D3D12_RESOURCE_DESC vd{};
            if (mirror_get_resource_desc(b.UAV.pResource, &vd) &&
                vision_layer_signature(vd) && vision_matches_last_dispatch(vd)) {
                const uintptr_t base = reinterpret_cast<uintptr_t>(g_exe_base);
                const uintptr_t work = t_current_node_work;
                const uint32_t rva = (base && work > base)
                    ? static_cast<uint32_t>(work - base) : 0;
                if (work != t_vision_node) { t_vision_node = work; t_vision_ord = 0; }
                const int32_t ord = t_vision_ord++;
                if (CyberpunkVR_VisionMap)
                    vision_note_surface(b.UAV.pResource, t_vrcam_node_active, rva, ord);
                // Only the second eye's is worth taking: MAIN composites its own. Copy it here,
                // while the transient is still alive -- the very next barrier batch recycles the
                // heap it sits in. Both extra conditions matter: without the size test the copy
                // alternated between the full and the half-res surface and leaked a texture per
                // alternation; without the ordinal it would take PS1040's output, not PS1213's.
                if (t_vrcam_node_active && rva == CyberpunkVR_VisionNode &&
                    ord == CyberpunkVR_VisionPick && vision_is_vrcam_full_size(vd)) {
                    hud_snapshot_copy(self, b.UAV.pResource, kSnapVision,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    g_vision_tick.store(GetTickCount64(), std::memory_order_release);
                    InterlockedIncrement64(
                        reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugVisionSnaps));
                }
            }
        }
        if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV && b.UAV.pResource) {
            D3D12_RESOURCE_DESC ud{};
            if (mirror_get_resource_desc(b.UAV.pResource, &ud) &&
                ud.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && ud.Width >= 4 &&
                ud.Width <= 32 &&
                (ud.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
                cull_count_note(self, b.UAV.pResource, static_cast<uint32_t>(ud.Width),
                                t_vrcam_node_active);
                cull_count_report();
            }
            continue;
        }
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
            b.Transition.StateBefore != D3D12_RESOURCE_STATE_UNORDERED_ACCESS ||
            b.Transition.StateAfter !=
                (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) ||
            !b.Transition.pResource) {
            continue;
        }
        D3D12_RESOURCE_DESC rd{};
        if (!mirror_get_resource_desc(b.Transition.pResource, &rd)) continue;
        // Same transition, different resource: the per-tile light grid the lighting pass indexes.
        // Taken here because this is the barrier that publishes it, so its state is known and not
        // guessed. (The probe was written earlier but never called -- dead code, hence silence.)
        if (tile_is_grid(rd)) {
            tile_probe_copy(self, b.Transition.pResource, rd, b.Transition.StateAfter,
                            t_vrcam_node_active);
            continue;
        }
        if (rd.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || rd.Width != 28) {
            continue;
        }
        if (t_vrcam_node_active) {
            ID3D12Resource* prev = g_expo_vrcam.exchange(
                b.Transition.pResource, std::memory_order_acq_rel);
            if (prev != b.Transition.pResource) {
                b.Transition.pResource->AddRef();
                if (prev) prev->Release();
            }
            CyberpunkVR_DebugExpoVrcam =
                reinterpret_cast<uint64_t>(b.Transition.pResource);
            expo_probe_copy(self, b.Transition.pResource, true);
            expo_mirror(self, b.Transition.pResource, true);
        } else {
            ID3D12Resource* prev = g_expo_main.exchange(
                b.Transition.pResource, std::memory_order_acq_rel);
            if (prev != b.Transition.pResource) {
                b.Transition.pResource->AddRef();
                if (prev) prev->Release();
            }
            CyberpunkVR_DebugExpoMain =
                reinterpret_cast<uint64_t>(b.Transition.pResource);
            expo_mirror(self, b.Transition.pResource, false);
            expo_probe_copy(self, b.Transition.pResource, false);
            expo_probe_report();
            tile_probe_report();
        }
    }
    // In-node, in-order tracking of the captured target's state (same recording
    // thread): gives the inline snapshot the EXACT StateBefore, no cross-frame guess.
    if (t_mirror_copy_node_active && t_mirror_copy_rtv) {
        for (UINT i = 0; i < count; ++i) {
            const D3D12_RESOURCE_BARRIER& b = barriers[i];
            if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
                b.Transition.pResource == t_mirror_copy_rtv) {
                t_mirror_src_state = (uint32_t)b.Transition.StateAfter;
            }
        }
    }
    // Track the tonemap RT0's real state so the epilogue snapshot uses the correct
    // StateBefore (else a hazard barrier stalls the engine's list = freeze).
    if (t_tm_rt0) {
        for (UINT i = 0; i < count; ++i) {
            const D3D12_RESOURCE_BARRIER& b = barriers[i];
            if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
                b.Transition.pResource == t_tm_rt0) {
                t_tm_rt0_state = (uint32_t)b.Transition.StateAfter;
            }
        }
    }
    ID3D12Resource* output = g_captured_vrcam_res.load(std::memory_order_acquire);
    if (!output) return;
    for (UINT i = 0; i < count; ++i) {
        const D3D12_RESOURCE_BARRIER& b = barriers[i];
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
            b.Transition.pResource != output) continue;
        CyberpunkVR_DebugMirrorSrcState = (uint32_t)b.Transition.StateAfter;
        InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(
            &CyberpunkVR_DebugMirrorBarrierHits));
    }
}

// Nsight cannot capture this setup, so the mod has to answer the question a capture would:
// what does each view actually upload into the light path? Summing the bytes was too blunt --
// it mixed every buffer the two nodes touch and produced a meaningless 0.86 ratio. A HISTOGRAM
// of upload sizes separates them: the light array is one distinctive size, and if VRCAM's copy
// of that size is smaller, or missing, that is the answer.
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_LightCensus = 1;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugLightBytesMain    = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugLightBytesVrcam   = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugLightUploadsMain  = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugLightUploadsVrcam = 0;

// The histogram found it: an 848-byte constant upload into a 1024-byte buffer, MAIN 1500 times
// and VRCAM never, while every real light array is uploaded equally by both. The question that
// decides the fix is whether VRCAM SHARES that buffer (then it reads MAIN's constants and this
// is a red herring) or owns an instance nothing ever fills (then it reads zeros). So record the
// destination resources each view touches, not just the sizes.
struct LightDst { void* res; uint64_t size; uint32_t hits[2]; uint32_t last_bytes; };
static std::array<LightDst, 24> g_light_dsts{};
static uint32_t g_light_dst_n = 0;

static void light_dst_note(void* dst, uint64_t size, uint32_t bytes, bool vrcam) {
    if (!dst || size > 4096) return;          // constant buffers only; the big arrays are noise
    uint32_t i = 0;
    for (; i < g_light_dst_n; ++i) if (g_light_dsts[i].res == dst) break;
    if (i == g_light_dst_n) {
        if (g_light_dst_n >= g_light_dsts.size()) return;
        g_light_dsts[g_light_dst_n++] = { dst, size, {0, 0}, bytes };
    }
    ++g_light_dsts[i].hits[vrcam ? 1 : 0];
    g_light_dsts[i].last_bytes = bytes;
}

struct LightSizeBin { uint32_t bytes; uint32_t hits[2]; uint64_t dst_size[2]; };
static std::array<LightSizeBin, 48> g_light_bins{};
static uint32_t g_light_bin_n = 0;
static std::mutex g_light_mtx;

static void light_census_note(uint64_t num_bytes, uint64_t dst_size, void* dst_res,
                              bool vrcam) {
    const uint32_t sz = static_cast<uint32_t>(num_bytes);
    std::lock_guard<std::mutex> lk(g_light_mtx);
    uint32_t i = 0;
    for (; i < g_light_bin_n; ++i) if (g_light_bins[i].bytes == sz) break;
    if (i == g_light_bin_n) {
        if (g_light_bin_n >= g_light_bins.size()) return;
        g_light_bins[g_light_bin_n++] = { sz, {0, 0}, {0, 0} };
    }
    ++g_light_bins[i].hits[vrcam ? 1 : 0];
    g_light_bins[i].dst_size[vrcam ? 1 : 0] = dst_size;
    light_dst_note(dst_res, dst_size, sz, vrcam);
}

// Reported as: upload size, how often each view makes it, and the destination buffer's own
// capacity. A size one view uploads and the other never does is exactly what we are hunting.
static void light_census_report() {
    static uint64_t s_last = 0;
    const uint64_t now = GetTickCount64();
    if (s_last && now - s_last < 12000) return;
    LightSizeBin bins[48];
    uint32_t n;
    {
        std::lock_guard<std::mutex> lk(g_light_mtx);
        n = g_light_bin_n;
        for (uint32_t i = 0; i < n; ++i) bins[i] = g_light_bins[i];
    }
    bool both = false;
    for (uint32_t i = 0; i < n; ++i) if (bins[i].hits[1]) { both = true; break; }
    if (!n || !both) return;
    s_last = now;
    // Biggest uploads first -- the light array dwarfs the little constant blocks.
    for (uint32_t a = 0; a + 1 < n; ++a)
        for (uint32_t b = a + 1; b < n; ++b)
            if (bins[b].bytes > bins[a].bytes) { LightSizeBin t = bins[a]; bins[a] = bins[b]; bins[b] = t; }
    char line[1500];
    int used = 0;
    line[0] = '\0';
    for (uint32_t i = 0; i < n && used < static_cast<int>(sizeof(line)) - 48; ++i) {
        const char* flag = (bins[i].hits[0] && !bins[i].hits[1]) ? "!MAIN-only"
                         : (!bins[i].hits[0] && bins[i].hits[1]) ? "!VRCAM-only" : "";
        used += snprintf(line + used, sizeof(line) - used, "%uB m%u/v%u dst%llu%s | ",
                         bins[i].bytes, bins[i].hits[0], bins[i].hits[1],
                         (unsigned long long)(bins[i].dst_size[0] ? bins[i].dst_size[0]
                                                                  : bins[i].dst_size[1]),
                         flag);
    }
    log("[lights] upload sizes inside ClusteredLightsCull+RenderLightBuffers: %s", line);
    char dl[900];
    int du = 0;
    dl[0] = 0;
    {
        std::lock_guard<std::mutex> lk(g_light_mtx);
        for (uint32_t i = 0; i < g_light_dst_n && du < static_cast<int>(sizeof(dl)) - 64; ++i) {
            const LightDst& d = g_light_dsts[i];
            du += snprintf(dl + du, sizeof(dl) - du, "%p sz%llu m%u/v%u last%uB%s | ",
                           d.res, (unsigned long long)d.size, d.hits[0], d.hits[1],
                           d.last_bytes,
                           (d.hits[0] && !d.hits[1]) ? " !MAIN-only"
                         : (!d.hits[0] && d.hits[1]) ? " !VRCAM-only" : "");
        }
    }
    log("[lights] constant-buffer destinations (<=4KB): %s", dl);
}

// ---- name the node behind each compute dispatch -------------------------------------------
// The night capture pinned the defect to one constant block: the pass at PSO 926 gets six
// world-space light entries and a non-zero count for MAIN, and an empty list with count 0 for
// VRCAM. What the capture cannot say is WHICH frame-graph node issues that dispatch, and
// without the node there is nothing to read in the reverse dumps and nothing to hook.
//
// The dispatch is identifiable by shape: it writes the tile grid, so its group count is the
// render resolution over 16 in both axes. Recording the node RVA for every distinct
// (groupX, groupY) per view names it in one run -- and gives the same table for every other
// compute pass for free.
extern "C" __declspec(dllexport) int32_t CyberpunkVR_DispatchCensus = 1;   // OFF: [dispatch] per-node dispatch census
struct DispatchBin { uint32_t node_rva, gx, gy, gz; uint32_t hits[2]; };
static std::array<DispatchBin, 96> g_disp_bins{};
static uint32_t g_disp_bin_n = 0;
static std::mutex g_disp_mtx;

static void dispatch_census_note(uint32_t rva, UINT x, UINT y, UINT z, bool vrcam) {
    std::lock_guard<std::mutex> lk(g_disp_mtx);
    uint32_t i = 0;
    for (; i < g_disp_bin_n; ++i) {
        const DispatchBin& b = g_disp_bins[i];
        if (b.node_rva == rva && b.gx == x && b.gy == y && b.gz == z) break;
    }
    if (i == g_disp_bin_n) {
        if (g_disp_bin_n >= g_disp_bins.size()) return;
        g_disp_bins[g_disp_bin_n++] = { rva, x, y, z, {0, 0} };
    }
    ++g_disp_bins[i].hits[vrcam ? 1 : 0];
}

// Report every dispatch shape ONE view issues and the other never does, whatever its size.
// The first cut of this filtered to square tile grids and so only found passes that were
// already understood (the HUD surface and its blur pyramid, our own shadow reuse) -- the
// filter, not the engine, is what hid everything else.
static void dispatch_census_report() {
    static uint64_t s_last = 0;
    const uint64_t now = GetTickCount64();
    if (s_last && now - s_last < 12000) return;
    // Both views have to have been seen at all, or every shape looks exclusive.
    bool anyv = false, anym = false;
    {
        std::lock_guard<std::mutex> lk(g_disp_mtx);
        for (uint32_t i = 0; i < g_disp_bin_n; ++i) {
            if (g_disp_bins[i].hits[0]) anym = true;
            if (g_disp_bins[i].hits[1]) anyv = true;
        }
    }
    if (!anym || !anyv) return;
    s_last = now;
    // Aggregate PER NODE, not per (node, shape). Thread-group counts are content-derived -- a
    // light or terrain-instance count -- so the two views almost never land on the same number,
    // and binning by the exact shape makes every such node look exclusive. That artefact is
    // what produced the earlier "ClusteredLightsCull 1964x1x1 is MAIN-only" reading: the
    // question is whether the node dispatches AT ALL for a view.
    struct NodeAgg { uint32_t rva; uint64_t hits[2]; uint32_t shapes[2]; };
    NodeAgg agg[96];
    uint32_t an = 0;
    {
        std::lock_guard<std::mutex> lk(g_disp_mtx);
        for (uint32_t i = 0; i < g_disp_bin_n; ++i) {
            const DispatchBin& b = g_disp_bins[i];
            uint32_t k = 0;
            for (; k < an; ++k) if (agg[k].rva == b.node_rva) break;
            if (k == an) { if (an >= 96) continue; agg[an++] = { b.node_rva, {0, 0}, {0, 0} }; }
            for (int s = 0; s < 2; ++s)
                if (b.hits[s]) { agg[k].hits[s] += b.hits[s]; ++agg[k].shapes[s]; }
        }
    }
    char mo[1200], vo[600];
    int mu = 0, vu = 0, mn = 0, vn = 0;
    mo[0] = 0; vo[0] = 0;
    for (uint32_t k = 0; k < an; ++k) {
        const NodeAgg& a = agg[k];
        if (a.hits[0] && !a.hits[1]) {
            ++mn;
            if (mu < static_cast<int>(sizeof(mo)) - 32)
                mu += snprintf(mo + mu, sizeof(mo) - mu, "%X(%llu) ", a.rva,
                               (unsigned long long)a.hits[0]);
        } else if (!a.hits[0] && a.hits[1]) {
            ++vn;
            if (vu < static_cast<int>(sizeof(vo)) - 32)
                vu += snprintf(vo + vu, sizeof(vo) - vu, "%X(%llu) ", a.rva,
                               (unsigned long long)a.hits[1]);
        }
    }
    log("[disp] nodes that dispatch for MAIN and NEVER for VRCAM (%d of %u): %s",
        mn, an, mo);
    log("[disp] nodes that dispatch for VRCAM and NEVER for MAIN (%d): %s", vn, vo);
}

// OFF -- the light arrays are byte-identical between the views.
extern "C" __declspec(dllexport) int32_t CyberpunkVR_LightContent = 0;
constexpr uint64_t LIGHT_SNAP_MAX = 64 * 1024;
static std::mutex g_lc_mtx;
static uint8_t  g_lc_buf[2][LIGHT_SNAP_MAX];
static uint32_t g_lc_len[2] = {0, 0};

// The upload ring is CPU-visible and already mapped for the cloud constants, so the bytes the
// engine is about to copy can be read here directly -- no readback, no extra GPU work.
static void light_content_note(ID3D12Resource* src, uint64_t off, uint64_t n, bool vrcam) {
    const uint8_t* p = upload_map_read(src);
    if (!p) return;
    uint8_t tmp[LIGHT_SNAP_MAX];
    if (!cloud_cb_raw_copy(tmp, p + off, static_cast<size_t>(n))) return;
    const int i = vrcam ? 1 : 0;
    {
        std::lock_guard<std::mutex> lk(g_lc_mtx);
        if (n <= g_lc_len[i]) return;              // keep the largest seen: that is the array
        memcpy(g_lc_buf[i], tmp, static_cast<size_t>(n));
        g_lc_len[i] = static_cast<uint32_t>(n);
    }
}

// Compare as 16-byte records, which is how the entries in MAIN's block decoded in the capture
// (world position + a scalar). Report the lengths, how many records differ, and how many look
// like real world-space positions in each -- a zeroed tail shows up immediately.
static void light_content_report() {
    static uint64_t s_last = 0;
    const uint64_t now = GetTickCount64();
    if (s_last && now - s_last < 12000) return;
    uint8_t a[LIGHT_SNAP_MAX], b[LIGHT_SNAP_MAX];
    uint32_t la, lb;
    {
        std::lock_guard<std::mutex> lk(g_lc_mtx);
        la = g_lc_len[0]; lb = g_lc_len[1];
        if (!la || !lb) return;
        memcpy(a, g_lc_buf[0], la); memcpy(b, g_lc_buf[1], lb);
    }
    s_last = now;
    auto worldish = [](const uint8_t* p, uint32_t len) {
        uint32_t c = 0;
        for (uint32_t o = 0; o + 12 <= len; o += 16) {
            float f[3]; memcpy(f, p + o, 12);
            if (fabsf(f[0]) > 100.0f && fabsf(f[1]) > 100.0f && fabsf(f[2]) < 500.0f) ++c;
        }
        return c;
    };
    const uint32_t common = la < lb ? la : lb;
    uint32_t diff = 0, first = 0xFFFFFFFF;
    for (uint32_t o = 0; o + 16 <= common; o += 16)
        if (memcmp(a + o, b + o, 16)) { ++diff; if (first == 0xFFFFFFFF) first = o; }
    log("[lightbuf] MAIN %u B (%u world-ish recs) | VRCAM %u B (%u) | of %u common recs %u differ,"
        " first at +%X", la, worldish(a, la), lb, worldish(b, lb), common / 16, diff,
        first == 0xFFFFFFFF ? 0 : first);
    // WHICH field differs decides everything. A lane that differs by ~0.064 is a world position
    // shifted by the IPD and is correct; a lane that differs wildly is view-derived data
    // computed wrong for VRCAM -- e.g. a screen-space bound or tile range, which would put the
    // lights in the wrong tiles and leave them unlit while the list itself looks perfect.
    double maxd[4] = {0, 0, 0, 0};
    uint32_t dcnt[4] = {0, 0, 0, 0};
    for (uint32_t o = 0; o + 16 <= common; o += 16) {
        for (int L = 0; L < 4; ++L) {
            float x, y;
            memcpy(&x, a + o + L * 4, 4);
            memcpy(&y, b + o + L * 4, 4);
            if (memcmp(a + o + L * 4, b + o + L * 4, 4)) {
                ++dcnt[L];
                const double d = fabs((double)x - (double)y);
                if (d > maxd[L] && d < 1e30) maxd[L] = d;
            }
        }
    }
    log("[lightbuf] per-lane: L0 %u diff max %.4g | L1 %u max %.4g | L2 %u max %.4g |"
        " L3 %u max %.4g", dcnt[0], maxd[0], dcnt[1], maxd[1], dcnt[2], maxd[2],
        dcnt[3], maxd[3]);
    char r0[700];
    int u0 = 0;
    r0[0] = 0;
    for (uint32_t k = 0; k < 5 && (k + 1) * 16 <= common; ++k) {
        float fa[4], fb[4];
        memcpy(fa, a + k * 16, 16);
        memcpy(fb, b + k * 16, 16);
        if (u0 < (int)sizeof(r0) - 160)
            u0 += snprintf(r0 + u0, sizeof(r0) - u0,
                           "[%u] M(%.4g %.4g %.4g %.4g) V(%.4g %.4g %.4g %.4g)  ", k,
                           fa[0], fa[1], fa[2], fa[3], fb[0], fb[1], fb[2], fb[3]);
    }
    log("[lightbuf] first records %s", r0);
}

// ---- is the two views' auto-exposure the same? ---------------------------------------------
// The lighting output has the lamps in BOTH views (checked in the capture), and every node after
// it -- ApplyBloomAndTonemapping, GenerateTonemappingLUT, HistogramUpdate -- runs symmetrically.
// What those share as an input but hold PER VIEW is the exposure: each view has its own 28-byte
// FrameExposureData. A higher exposure for VRCAM would compress the highlights and keep the
// lamps under the bloom threshold -- light present, glow absent, which is exactly the symptom.
// Read here rather than inferred: the buffer is copied into a readback in the engine's own list
// at the barrier that already identifies it, so no state is guessed and nothing is added to any
// queue of ours.
// OFF -- exposure differs by 10-35%: real, but recorded, not being re-measured.
extern "C" __declspec(dllexport) int32_t CyberpunkVR_ExpoProbe = 1;
extern "C" __declspec(dllexport) float   CyberpunkVR_DebugExpoValMain  = 0.f;
extern "C" __declspec(dllexport) float   CyberpunkVR_DebugExpoValVrcam = 0.f;
static ID3D12Resource* g_expo_rb[2] = {nullptr, nullptr};
static uint8_t*        g_expo_rb_map[2] = {nullptr, nullptr};

static bool expo_rb_ensure(int v) {
    if (g_expo_rb[v]) return g_expo_rb_map[v] != nullptr;
    if (!g_game_device) return false;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    hp.CreationNodeMask = hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 256;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb = nullptr;
    if (FAILED(g_game_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb))) || !rb) return false;
    void* m = nullptr;
    D3D12_RANGE none{0, 0};
    if (FAILED(rb->Map(0, &none, &m)) || !m) { rb->Release(); return false; }
    rb->SetName(v ? L"CyberpunkVR_ExpoRbVrcam" : L"CyberpunkVR_ExpoRbMain");
    g_expo_rb[v] = rb;
    g_expo_rb_map[v] = static_cast<uint8_t*>(m);
    return true;
}

// ---- THE CLOUD BRIGHTNESS, and why it is only the clouds ------------------------------------
//
// Nsight settles it. The cloud raymarch is PipelineState_1301, an indirect dispatch in
// Transparents that read-writes the half-res cloud buffer, and its last line is:
//
//     float _471 = WaveReadLaneFirst(asfloat(_8.Load(0u).x));      // _8 = t37
//     _23[pixel] = float4(_471 * r, _471 * g, _471 * b, alpha);
//
// and t37 is bound to `StructuredBuffer<FrameExposureData>` -- Resource_30946 on the second view,
// Resource_2803 on MAIN. The clouds are multiplied by THAT VIEW'S EXPOSURE at raymarch time.
// Alpha is left alone.
//
// Everything else in the frame takes exposure later, at tonemap, where each view applies its own
// consistently. The clouds are the only thing that bakes it early, so a per-view exposure
// difference lands on the clouds and nowhere else -- a flat multiplier, not noise, which is
// exactly the reported "VRCAM's clouds are always lighter".
//
// And the difference was already measured by the probe below: 10-35%.
//
// Two eyes 6.5 cm apart have no business metering differently -- unequal brightness between eyes
// is binocular rivalry, tiring even when it is not consciously noticed. So this is a correction,
// not a workaround: hand the second view MAIN's exposure.
//
// Done with the probe's own proven mechanism. The buffer is copied at the barrier that hands it
// to the shaders, so its state is known exactly, and the copy goes through a private staging
// buffer rather than touching MAIN's resource from the second view's list: MAIN's copy is taken
// where MAIN's state is known, VRCAM's is written where VRCAM's is. Frame order is VRCAM-first,
// so the value applied is MAIN's from the previous frame -- exposure adapts over many frames, so
// one frame of lag is nothing.
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_ExpoMirror = 1;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugExpoMirrors = 0;
static ID3D12Resource* g_expo_stage = nullptr;

static bool expo_stage_ensure() {
    if (g_expo_stage) return true;
    if (!g_game_device) return false;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 256;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    if (FAILED(g_game_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r))) || !r) return false;
    r->SetName(L"CyberpunkVR_ExpoStage");
    g_expo_stage = r;
    return true;
}

static void expo_mirror(ID3D12GraphicsCommandList* list, ID3D12Resource* src, bool vrcam) {
    if (!CyberpunkVR_ExpoMirror || !list || !src) return;
    if (!expo_stage_ensure()) return;
    const CommandListVtableHook* e = command_list_hook_entry(list);
    if (!e || !e->barrier_call || !e->cbr_original) return;
    const D3D12_RESOURCE_STATES kSrv = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = src;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (!vrcam) {
        // MAIN: park a copy of its exposure in our staging buffer.
        b.Transition.StateBefore = kSrv;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        e->barrier_call(list, 1, &b);
        e->cbr_original(list, g_expo_stage, 0, src, 0, 28);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter  = kSrv;
        e->barrier_call(list, 1, &b);
        return;
    }
    // VRCAM: overwrite its exposure with MAIN's.
    D3D12_RESOURCE_BARRIER s{};
    s.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    s.Transition.pResource = g_expo_stage;
    s.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    s.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    e->barrier_call(list, 1, &s);
    b.Transition.StateBefore = kSrv;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    e->barrier_call(list, 1, &b);
    e->cbr_original(list, src, 0, g_expo_stage, 0, 28);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter  = kSrv;
    e->barrier_call(list, 1, &b);
    s.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    s.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    e->barrier_call(list, 1, &s);
    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugExpoMirrors));
}


// Appended to the engine's own list at the barrier that hands the buffer to the shaders, so its
// state is known exactly: it is going to *_SHADER_RESOURCE, which is where a copy source is legal.
static void expo_probe_copy(ID3D12GraphicsCommandList* list, ID3D12Resource* src, bool vrcam) {
    if (!CyberpunkVR_ExpoProbe || !list || !src) return;
    const int v = vrcam ? 1 : 0;
    if (!expo_rb_ensure(v)) return;
    const CommandListVtableHook* e = command_list_hook_entry(list);
    if (!e || !e->barrier_call || !e->cbr_original) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = src;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    e->barrier_call(list, 1, &b);
    e->cbr_original(list, g_expo_rb[v], 0, src, 0, 28);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    e->barrier_call(list, 1, &b);
}

static void expo_probe_report() {
    static uint64_t s_last = 0;
    if (!g_expo_rb_map[0] || !g_expo_rb_map[1]) return;
    const uint64_t now = GetTickCount64();
    if (s_last && now - s_last < 8000) return;
    s_last = now;
    float m[7], v[7];
    memcpy(m, g_expo_rb_map[0], 28);
    memcpy(v, g_expo_rb_map[1], 28);
    CyberpunkVR_DebugExpoValMain  = m[6];
    CyberpunkVR_DebugExpoValVrcam = v[6];
    log("[expo] MAIN %.6g %.6g %.6g %.6g %.6g %.6g %.6g | VRCAM %.6g %.6g %.6g %.6g %.6g %.6g %.6g",
        m[0], m[1], m[2], m[3], m[4], m[5], m[6],
        v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
}

// ---- does the light cull actually FILL the tile grid for VRCAM? ----------------------------
// The lamp's own emissive surface shows in both views; the light it casts does not -- no red
// wash under the red lamps, no lit roof, no lit road. In a clustered renderer that is exactly
// "the light list is fine but the per-tile assignment is empty", and the list IS fine: compared
// byte for byte, same length, same entries. So read the cull's OUTPUT -- the R16_UINT grid at
// render-res/32 that the lighting pass indexes. Non-zero tiles for MAIN and zeros for VRCAM
// would settle this; equal grids move the search to the lighting shader's other inputs.
// OFF -- the per-tile grids read equal once the readback was finally synchronised.
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_TileProbe = 1;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugTileNonzeroMain  = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugTileNonzeroVrcam = 0;

// One signature matches several different integer grids, and the first version silently mixed
// them: MAIN reported a stable 160x160 (max 5, i.e. a per-tile light COUNT) while VRCAM reported
// a 93x160 with wildly different statistics -- a different resource, not a different result.
// So keep every distinct grid separately, keyed by its dimensions, and report them all. Only
// grids of the SAME shape are comparable between the views.
// A single readback buffer read on a timer is worthless: the copy is recorded into the ENGINE'S
// list and nothing waits for the GPU, so the report shows a half-written or stale grid -- which
// is how MAIN's grid read full in one run and empty in the next while the symptom never moved.
// Three buffers, written round-robin, and only the OLDEST is read: by then two more copies have
// been recorded behind it, so the one being read is long since retired.
struct TileGrid {
    uint32_t w, h, pitch;
    DXGI_FORMAT fmt;
    ID3D12Resource* rb[3];
    uint8_t* map[3];
    uint32_t writes;
    uint64_t nz, mx, sum;
    bool seen;
};
static std::array<TileGrid, 8> g_tiles[2]{};
static uint32_t g_tile_n[2] = {0, 0};

static bool tile_is_grid(const D3D12_RESOURCE_DESC& d) {
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return false;
    if (d.MipLevels != 1 || d.DepthOrArraySize != 1) return false;
    if (!(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) return false;
    if (d.Width < 40 || d.Width > 220 || d.Height < 40 || d.Height > 220) return false;
    return d.Format == DXGI_FORMAT_R16_UINT || d.Format == DXGI_FORMAT_R32_UINT ||
           d.Format == DXGI_FORMAT_R8_UINT  || d.Format == DXGI_FORMAT_R32G32_UINT;
}

static TileGrid* tile_slot(int v, const D3D12_RESOURCE_DESC& d) {
    const uint32_t w = static_cast<uint32_t>(d.Width);
    for (uint32_t i = 0; i < g_tile_n[v]; ++i) {
        TileGrid& t = g_tiles[v][i];
        if (t.w == w && t.h == d.Height && t.fmt == d.Format) return &t;
    }
    if (g_tile_n[v] >= g_tiles[v].size() || !g_game_device) return nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    g_game_device->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb[3] = {nullptr, nullptr, nullptr};
    uint8_t* mp[3] = {nullptr, nullptr, nullptr};
    D3D12_RANGE none{0, 0};
    for (int k = 0; k < 3; ++k) {
        if (FAILED(g_game_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb[k]))) || !rb[k]) {
            for (int j = 0; j < k; ++j) rb[j]->Release();
            return nullptr;
        }
        void* m = nullptr;
        if (FAILED(rb[k]->Map(0, &none, &m)) || !m) {
            for (int j = 0; j <= k; ++j) rb[j]->Release();
            return nullptr;
        }
        mp[k] = static_cast<uint8_t*>(m);
    }
    TileGrid& t = g_tiles[v][g_tile_n[v]++];
    t.w = w; t.h = d.Height; t.pitch = fp.Footprint.RowPitch; t.fmt = d.Format;
    for (int k = 0; k < 3; ++k) { t.rb[k] = rb[k]; t.map[k] = mp[k]; }
    t.writes = 0;
    t.nz = t.mx = t.sum = 0; t.seen = false;
    return &t;
}

// Appended at the barrier that publishes the grid, so the source state is exact rather than
// assumed -- the same discipline as every other inline copy in this file.
static void tile_probe_copy(ID3D12GraphicsCommandList* list, ID3D12Resource* src,
                            const D3D12_RESOURCE_DESC& d, D3D12_RESOURCE_STATES after,
                            bool vrcam) {
    if (!CyberpunkVR_TileProbe || !list || !src || !g_game_device) return;
    const int v = vrcam ? 1 : 0;
    TileGrid* t = tile_slot(v, d);
    if (!t) return;
    const CommandListVtableHook* e = command_list_hook_entry(list);
    if (!e || !e->barrier_call || !e->copytex) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = src;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = after;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    e->barrier_call(list, 1, &b);
    D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
    dl.pResource = t->rb[t->writes % 3];
    dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dl.PlacedFootprint.Offset = 0;
    dl.PlacedFootprint.Footprint.Format = d.Format;
    dl.PlacedFootprint.Footprint.Width = t->w;
    dl.PlacedFootprint.Footprint.Height = t->h;
    dl.PlacedFootprint.Footprint.Depth = 1;
    dl.PlacedFootprint.Footprint.RowPitch = t->pitch;
    sl.pResource = src;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sl.SubresourceIndex = 0;
    e->copytex(list, &dl, 0, 0, 0, &sl, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = after;
    e->barrier_call(list, 1, &b);
    ++t->writes;
    t->seen = true;
}

static void tile_scan(TileGrid& t) {
    // writes-3 is the oldest of the three, i.e. two further copies were recorded after it.
    const uint8_t* base = t.map[(t.writes + 0) % 3];
    const uint32_t stride = (t.fmt == DXGI_FORMAT_R8_UINT)     ? 1u
                          : (t.fmt == DXGI_FORMAT_R16_UINT)    ? 2u
                          : (t.fmt == DXGI_FORMAT_R32G32_UINT) ? 8u : 4u;
    t.nz = t.mx = t.sum = 0;
    for (uint32_t y = 0; y < t.h; ++y) {
        const uint8_t* row = base + static_cast<size_t>(y) * t.pitch;
        for (uint32_t x = 0; x < t.w; ++x) {
            uint64_t val = 0;
            memcpy(&val, row + static_cast<size_t>(x) * stride, stride > 8 ? 8 : stride);
            if (val) { ++t.nz; t.sum += val; if (val > t.mx) t.mx = val; }
        }
    }
}

static void tile_probe_report() {
    static uint64_t s_last = 0;
    const uint64_t now = GetTickCount64();
    if (s_last && now - s_last < 8000) return;
    if (!g_tile_n[0] || !g_tile_n[1]) return;
    s_last = now;
    for (int v = 0; v < 2; ++v) {
        char line[900];
        int u = 0;
        line[0] = 0;
        for (uint32_t i = 0; i < g_tile_n[v]; ++i) {
            TileGrid& t = g_tiles[v][i];
            if (!t.seen || t.writes < 3) continue;   // ring not filled yet: nothing retired
            tile_scan(t);
            if (u < static_cast<int>(sizeof(line)) - 80)
                u += snprintf(line + u, sizeof(line) - u,
                              "%ux%u fmt%u nz%llu max%llu sum%llu | ", t.w, t.h,
                              static_cast<unsigned>(t.fmt), (unsigned long long)t.nz,
                              (unsigned long long)t.mx, (unsigned long long)t.sum);
            if (v == 0) CyberpunkVR_DebugTileNonzeroMain = t.nz;
            else        CyberpunkVR_DebugTileNonzeroVrcam = t.nz;
        }
        log("[tile] %-5s grids: %s", v ? "VRCAM" : "MAIN", u ? line : "(none)");
    }
}

// ---- how many lights does each view's cull actually output? --------------------------------
// The capture shows both views clearing and filling the SAME persistent UAV buffers for the
// clustered light list -- Resource_1360 (20 B counter), 2980/2981 (3 MB each), 2991, 2994, 4943 --
// while some neighbouring counters are per-view transients. A 20-byte counter is the cheapest
// thing in that set to read, and it separates the two remaining explanations outright:
//   count == 0 for VRCAM  -> its cull produces nothing, and the fault is in the cull's inputs;
//   count  > 0 for VRCAM  -> the cull works and the result is overwritten before its lighting
//                            reads it, i.e. the shared buffers are raced (AsyncComputeDuring-
//                            Shadowmaps is a separate list and a likely second writer).
// Read off a UAV barrier, not a transition: this counter never changes state, it stays in
// UNORDERED_ACCESS, so the copy brackets it explicitly from a state we know rather than guess.
// OFF -- the 20-byte counters it found were indirect-draw args, now understood.
extern "C" __declspec(dllexport) int32_t CyberpunkVR_CullCountProbe = 1;
struct CullCnt {
    ID3D12Resource* res;
    ID3D12Resource* rb[3];
    uint8_t*        map[3];
    uint32_t        writes;
    uint32_t        bytes;
    uint32_t        last[2][5];
    bool            seen[2];
};
static std::array<CullCnt, 6> g_cull{};
static uint32_t g_cull_n = 0;

static CullCnt* cull_slot(ID3D12Resource* res, uint32_t bytes) {
    for (uint32_t i = 0; i < g_cull_n; ++i) if (g_cull[i].res == res) return &g_cull[i];
    if (g_cull_n >= g_cull.size() || !g_game_device) return nullptr;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 256;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    CullCnt& c = g_cull[g_cull_n];
    D3D12_RANGE none{0, 0};
    for (int k = 0; k < 3; ++k) {
        ID3D12Resource* rb = nullptr;
        if (FAILED(g_game_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb))) || !rb) {
            for (int j = 0; j < k; ++j) c.rb[j]->Release();
            return nullptr;
        }
        void* m = nullptr;
        if (FAILED(rb->Map(0, &none, &m)) || !m) {
            rb->Release();
            for (int j = 0; j < k; ++j) c.rb[j]->Release();
            return nullptr;
        }
        c.rb[k] = rb;
        c.map[k] = static_cast<uint8_t*>(m);
    }
    c.res = res;
    c.bytes = bytes > 20 ? 20 : bytes;
    c.writes = 0;
    memset(c.last, 0, sizeof(c.last));
    c.seen[0] = c.seen[1] = false;
    ++g_cull_n;
    return &c;
}

static void cull_count_note(ID3D12GraphicsCommandList* list, ID3D12Resource* res,
                            uint32_t bytes, bool vrcam) {
    if (!CyberpunkVR_CullCountProbe || !list || !res) return;
    CullCnt* c = cull_slot(res, bytes);
    if (!c) return;
    const CommandListVtableHook* e = command_list_hook_entry(list);
    if (!e || !e->barrier_call || !e->cbr_original) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    e->barrier_call(list, 1, &b);
    e->cbr_original(list, c->rb[c->writes % 3], 0, res, 0, c->bytes);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    e->barrier_call(list, 1, &b);
    ++c->writes;
    c->seen[vrcam ? 1 : 0] = true;
    // Snapshot into the per-view row from the OLDEST ring entry, which the GPU has long retired.
    if (c->writes >= 3) {
        const uint8_t* oldest = c->map[c->writes % 3];
        memcpy(c->last[vrcam ? 1 : 0], oldest, c->bytes);
    }
}

static void cull_count_report() {
    static uint64_t s_last = 0;
    const uint64_t now = GetTickCount64();
    if (s_last && now - s_last < 8000) return;
    bool both = false;
    for (uint32_t i = 0; i < g_cull_n; ++i) if (g_cull[i].seen[0] && g_cull[i].seen[1]) both = true;
    if (!g_cull_n) return;
    s_last = now;
    char line[1000];
    int u = 0;
    line[0] = 0;
    for (uint32_t i = 0; i < g_cull_n && u < static_cast<int>(sizeof(line)) - 90; ++i) {
        const CullCnt& c = g_cull[i];
        u += snprintf(line + u, sizeof(line) - u,
                      "%p(%uB,%s%s) M[%u %u %u %u %u] V[%u %u %u %u %u] | ", c.res, c.bytes,
                      c.seen[0] ? "m" : "-", c.seen[1] ? "v" : "-",
                      c.last[0][0], c.last[0][1], c.last[0][2], c.last[0][3], c.last[0][4],
                      c.last[1][0], c.last[1][1], c.last[1][2], c.last[1][3], c.last[1][4]);
    }
    log("[cull] small UAV counters (shared ones carry both m and v): %s%s", line,
        both ? "" : "  [no counter seen by both views yet]");
}

// ---- the work the Dispatch census could never see -------------------------------------------
// The lighting is issued through ExecuteIndirect, which is vtable slot 59 -- an entirely
// different call from Dispatch (slot 14). Every census so far was blind to it, which is why they
// all reported the lighting nodes as symmetric while the light was plainly missing.
//
// The capture says what to expect: inside the Lighting list MAIN issues one
// CommandSignature_81 / Resource_1359 indirect DRAW (that signature is used 126 further times in
// GBuffer_Discard and the shadow cascades, so it is a draw, not a dispatch -- i.e. light volumes)
// and VRCAM issues none, while every other signature/argument pair matches one for one.
extern "C" __declspec(dllexport) int32_t CyberpunkVR_IndirectCensus = 1;   // OFF: [indirect] ExecuteIndirect census
struct IndirectBin { uint32_t node_rva; void* sig; uint64_t hits[2]; };
static std::array<IndirectBin, 64> g_ind{};
static uint32_t g_ind_n = 0;
static std::mutex g_ind_mtx;

static void indirect_report() {
    static uint64_t s_last = 0;
    const uint64_t now = GetTickCount64();
    if (s_last && now - s_last < 10000) return;
    IndirectBin b[64];
    uint32_t n;
    {
        std::lock_guard<std::mutex> lk(g_ind_mtx);
        n = g_ind_n;
        for (uint32_t i = 0; i < n; ++i) b[i] = g_ind[i];
    }
    bool anym = false, anyv = false;
    for (uint32_t i = 0; i < n; ++i) { if (b[i].hits[0]) anym = true; if (b[i].hits[1]) anyv = true; }
    if (!anym || !anyv) return;
    s_last = now;
    char mo[900], vo[900];
    int mu = 0, vu = 0;
    mo[0] = 0; vo[0] = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (b[i].hits[0] && !b[i].hits[1] && mu < static_cast<int>(sizeof(mo)) - 48)
            mu += snprintf(mo + mu, sizeof(mo) - mu, "%X/sig%p(%llu) ", b[i].node_rva, b[i].sig,
                           (unsigned long long)b[i].hits[0]);
        if (!b[i].hits[0] && b[i].hits[1] && vu < static_cast<int>(sizeof(vo)) - 48)
            vu += snprintf(vo + vu, sizeof(vo) - vu, "%X/sig%p(%llu) ", b[i].node_rva, b[i].sig,
                           (unsigned long long)b[i].hits[1]);
    }
    log("[indirect] node/signature pairs MAIN issues and VRCAM never: %s", mu ? mo : "(none)");
    log("[indirect] node/signature pairs VRCAM issues and MAIN never: %s", vu ? vo : "(none)");
}

// ---- which PSO draws the holographic sight ---------------------------------------------------
// The reticle is a 6-index instanced quad (PipelineState_29513 in the capture, Forward_NoTXAA,
// depth range 0.9..1.0 -- the first-person weapon layer). To give it real collimated behaviour
// its pixel shader has to be replaced, and a shader can only be swapped where the PSO is CREATED.
// So creation needs a stable name for it. The PSO pointer is not stable across runs; the PS
// bytecode hash is, being a hash of the bytes themselves.
//
// This pass only NAMES it. Nothing is substituted yet and no rendering changes.
extern "C" __declspec(dllexport) int32_t CyberpunkVR_PsoProbe = 1;   // OFF: both hashes are
// known and hardcoded now, and this runs on every 6-index instanced draw -- of which a menu
// issues tens of thousands. Set to 1 to re-identify a shader.
// Identification by REMOVAL. Two pixel shaders draw a 6-index instanced quad once per view per
// frame inside CRenderNode_RenderElements, and their tallies are equally balanced, so counting
// cannot separate them. Setting this to one of the two hashes drops that draw: whichever makes
// the reticle disappear IS the reticle. Instant, reversible, and it also proves the whole
// identification chain end to end before anything is substituted for real.
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_SightSkipPs = 0;
// CONFIRMED by removal, 2026-07-29: dropping this pixel shader's draw removes the reticle, and
// dropping the other 6-index quad in the same node (FA07D39A5AFDFBA5) changes nothing visible.
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_SightPsHash = 0x66394C5F4B95AB9Aull;
// One-shot: write the original bytecode out, so the replacement can be built against the real
// container -- shader model, input signature and whether this is DXBC or a DXIL blob decide
// whether it is fxc or dxc that has to compile it. Guessing that would waste a build.
extern "C" __declspec(dllexport) int32_t CyberpunkVR_SightPsDump = 1;   // one-shot, long done
struct SightVs { uint64_t hash; std::vector<uint8_t> bytes; bool written = false; };
static std::vector<SightVs> g_sight_vs;
static std::mutex g_sight_vs_mtx;
// Published by the DRAW: the vertex shader the sight actually runs with.
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugSightVsUsed = 0;

static void sight_ps_dump(const void* bytes, size_t len, const char* name) {
    char path[MAX_PATH]{};
    if (!GetModuleFileNameA(nullptr, path, MAX_PATH)) return;
    char* slash = strrchr(path, '\\');
    if (!slash) return;
    *(slash + 1) = '\0';
    strcat_s(path, name);
    HANDLE f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(f, bytes, static_cast<DWORD>(len), &wrote, nullptr);
    CloseHandle(f);
    // Chunk list straight from the DXBC container header, so the format is known from the log
    // alone: SHEX/SHDR means DXBC (fxc), a DXIL chunk means shader model 6 (dxc).
    char tags[256]; int u = 0; tags[0] = 0;
    __try {
        const uint8_t* b = static_cast<const uint8_t*>(bytes);
        if (len > 32 && memcmp(b, "DXBC", 4) == 0) {
            const uint32_t n = *reinterpret_cast<const uint32_t*>(b + 28);
            const uint32_t* offs = reinterpret_cast<const uint32_t*>(b + 32);
            for (uint32_t i = 0; i < n && i < 24; ++i) {
                if (offs[i] + 4 > len) break;
                if (u < 240) u += snprintf(tags + u, sizeof(tags) - u, "%.4s ", b + offs[i]);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { tags[u] = 0; }
    log("[pso] sight shader dumped %zu bytes -> %s   chunks: %s", len, path, tags);
}
struct PsoIds { uint64_t ps, vs; uint32_t ps_len, vs_len; };
static std::unordered_map<void*, PsoIds> g_pso_ids;
static std::mutex g_pso_ids_mtx;
static thread_local ID3D12PipelineState* t_current_pso = nullptr;

static uint64_t fnv1a(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

static void pso_ids_record(void* pso, const D3D12_SHADER_BYTECODE& ps,
                           const D3D12_SHADER_BYTECODE& vs) {
    if (!pso) return;
    PsoIds ids{};
    if (ps.pShaderBytecode && ps.BytecodeLength) {
        ids.ps = fnv1a(ps.pShaderBytecode, ps.BytecodeLength);
        ids.ps_len = static_cast<uint32_t>(ps.BytecodeLength);
    }
    if (vs.pShaderBytecode && vs.BytecodeLength) {
        ids.vs = fnv1a(vs.pShaderBytecode, vs.BytecodeLength);
        ids.vs_len = static_cast<uint32_t>(vs.BytecodeLength);
    }
    if (!ids.ps && !ids.vs) return;
    // More than one pipeline uses this pixel shader, and the first one created is NOT the one the
    // sight draws with: the first dump came back with a 3-input vertex shader (POSITION/TEXCOORD/
    // COLOR), which cannot even place a quad in the world. So every vertex shader paired with this
    // pixel shader is cached here, and the DRAW decides afterwards which of them is the real one.
    if (ids.ps == CyberpunkVR_SightPsHash && vs.pShaderBytecode && vs.BytecodeLength) {
        std::lock_guard<std::mutex> lk(g_sight_vs_mtx);
        bool have = false;
        for (auto& e : g_sight_vs) if (e.hash == ids.vs) { have = true; break; }
        if (!have && g_sight_vs.size() < 8) {
            SightVs e;
            e.hash = ids.vs;
            e.bytes.assign(static_cast<const uint8_t*>(vs.pShaderBytecode),
                           static_cast<const uint8_t*>(vs.pShaderBytecode) + vs.BytecodeLength);
            g_sight_vs.push_back(std::move(e));
            log("[pso] sight-PS pipeline #%zu uses VS %016llX (%zu bytes)",
                g_sight_vs.size(), (unsigned long long)ids.vs, vs.BytecodeLength);
        }
    }
    if (CyberpunkVR_SightPsDump && ids.ps == CyberpunkVR_SightPsHash) {
        static std::atomic<bool> s_done{false};
        bool expected = false;
        if (s_done.compare_exchange_strong(expected, true)) {
            sight_ps_dump(ps.pShaderBytecode, ps.BytecodeLength, "cyberpunkvr_sight_ps.bin");
            // (The vertex shader is NOT written here -- see the cache above.)
        }
    }
    std::lock_guard<std::mutex> lk(g_pso_ids_mtx);
    if (g_pso_ids.size() < 65536) g_pso_ids[pso] = ids;
}

// Candidates, tallied. The sight draws exactly ONCE per view per frame, so the row whose hit
// count tracks the frame count is it -- the tally is the check, not a guess from one sighting.
struct SightCand { uint64_t ps; uint32_t node_rva, ps_len; uint64_t hits[2]; };
static std::array<SightCand, 32> g_sight{};
static uint32_t g_sight_n = 0;
static std::mutex g_sight_mtx;

static void sight_report() {
    static uint64_t s_last = 0;
    const uint64_t now = GetTickCount64();
    if (s_last && now - s_last < 20000) return;
    SightCand c[32];
    uint32_t n;
    {
        std::lock_guard<std::mutex> lk(g_sight_mtx);
        n = g_sight_n;
        for (uint32_t i = 0; i < n; ++i) c[i] = g_sight[i];
    }
    if (!n) return;
    s_last = now;
    char line[1200];
    int u = 0;
    line[0] = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (u < static_cast<int>(sizeof(line)) - 70)
            u += snprintf(line + u, sizeof(line) - u, "ps=%016llX(%u)@%X M%llu/V%llu  ",
                          (unsigned long long)c[i].ps, c[i].ps_len, c[i].node_rva,
                          (unsigned long long)c[i].hits[0], (unsigned long long)c[i].hits[1]);
    }
    log("[psoprobe] 6-index instanced quads (%u): %s", n, line);
}

// Returns the PS hash of the draw, 0 when unknown -- the caller uses it to act, not just count.
static uint64_t sight_note(bool vrcam) {
    ID3D12PipelineState* pso = t_current_pso;
    if (!pso) return 0;
    PsoIds ids{};
    {
        std::lock_guard<std::mutex> lk(g_pso_ids_mtx);
        auto it = g_pso_ids.find(pso);
        if (it == g_pso_ids.end()) return 0;
        ids = it->second;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(g_exe_base);
    const uintptr_t work = t_current_node_work;
    const uint32_t rva = (base && work > base) ? static_cast<uint32_t>(work - base) : 0;
    {
        std::lock_guard<std::mutex> lk(g_sight_mtx);
        uint32_t i = 0;
        for (; i < g_sight_n; ++i) if (g_sight[i].ps == ids.ps && g_sight[i].node_rva == rva) break;
        if (i == g_sight_n) {
            if (g_sight_n >= g_sight.size()) return ids.ps;
            g_sight[g_sight_n++] = { ids.ps, rva, ids.ps_len, {0, 0} };
        }
        ++g_sight[i].hits[vrcam ? 1 : 0];
    }
    if (ids.ps == CyberpunkVR_SightPsHash && ids.vs) {
        CyberpunkVR_DebugSightVsUsed = ids.vs;
        std::lock_guard<std::mutex> lk(g_sight_vs_mtx);
        for (auto& e : g_sight_vs) {
            if (e.hash != ids.vs || e.written) continue;
            e.written = true;
            sight_ps_dump(e.bytes.data(), e.bytes.size(), "cyberpunkvr_sight_vs.bin");
            log("[pso] sight VS IN USE %016llX (%zu bytes) -- that is the one to replace",
                (unsigned long long)e.hash, e.bytes.size());
        }
    }
    sight_report();
    return ids.ps;
}

// The instance buffer sits on a DEFAULT heap -- the first read attempt reported exactly that --
// so it cannot be mapped. But the engine FILLS it with CopyBufferRegion from an upload buffer,
// and that source IS mappable. Remembering the last few (destination range -> source bytes) pairs
// therefore gives a CPU view of it with no new hooks and no readback plumbing.
// The instance buffer is neither mapped nor filled by a copy we can see -- the engine builds it
// on the GPU. So it has to be read back from the GPU, and for that the VA the vertex-buffer view
// carries must be resolved to a resource. There is no API for that, so buffers are recorded as
// they are created. Buffers are exempt from D3D12's state rules (always effectively COMMON, with
// implicit promotion), so the copy below needs no barriers on the engine's resource at all.
struct BufRange { uint64_t va; uint64_t size; ID3D12Resource* res; };
static std::array<BufRange, 512> g_bufs{};
static std::atomic<uint32_t> g_bufs_n{0};
static std::mutex g_bufs_mtx;

static void buf_note(ID3D12Resource* res, uint64_t va, uint64_t size) {
    if (!res || !va || !size) return;
    std::lock_guard<std::mutex> lk(g_bufs_mtx);
    const uint32_t n = g_bufs_n.load(std::memory_order_relaxed);
    if (n >= g_bufs.size()) return;
    g_bufs[n] = { va, size, res };
    g_bufs_n.store(n + 1, std::memory_order_release);
}

static ID3D12Resource* buf_for_va(uint64_t va, uint64_t need, uint64_t* off_out) {
    std::lock_guard<std::mutex> lk(g_bufs_mtx);
    const uint32_t n = g_bufs_n.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; ++i) {
        const BufRange& b = g_bufs[i];
        if (va >= b.va && va + need <= b.va + b.size) { *off_out = va - b.va; return b.res; }
    }
    return nullptr;
}

struct FilledRange { uint64_t va; uint64_t bytes; const uint8_t* cpu; };
static std::array<FilledRange, 32> g_filled{};
static std::atomic<uint32_t> g_filled_next{0};
static std::mutex g_filled_mtx;

static void filled_note(uint64_t va, uint64_t bytes, const uint8_t* cpu) {
    if (!va || !cpu || !bytes) return;
    std::lock_guard<std::mutex> lk(g_filled_mtx);
    const uint32_t i = g_filled_next.fetch_add(1, std::memory_order_relaxed) % g_filled.size();
    g_filled[i] = { va, bytes, cpu };
}

// SEH cannot share a frame with objects that unwind, and filled_note takes a lock -- hence the
// split. Same reason the stream walker and the grading commit are their own functions.
static void filled_note_guarded(ID3D12Resource* dst, uint64_t dst_off, uint64_t bytes,
                                const uint8_t* cpu) {
    uint64_t va = 0;
    __try { va = dst->GetGPUVirtualAddress(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (va) filled_note(va + dst_off, bytes, cpu);
}

static const uint8_t* filled_cpu_for_va(uint64_t va, uint64_t need) {
    std::lock_guard<std::mutex> lk(g_filled_mtx);
    for (const FilledRange& f : g_filled)
        if (f.cpu && va >= f.va && va + need <= f.va + f.bytes) return f.cpu + (va - f.va);
    return nullptr;
}

// The instance stream, per recording thread.
static thread_local uint64_t t_inst_va = 0;
static thread_local uint32_t t_inst_stride = 0;
// OFF: a GetGPUVirtualAddress() call and a mutex on every CopyBufferRegion, every descriptor
// table bind and every resource creation -- thousands a frame. It answered its question; the
// cost it leaves behind is uneven frame time.
extern "C" __declspec(dllexport) int32_t CyberpunkVR_SightAxisProbe = 1;

static void STDMETHODCALLTYPE hk_IASetVertexBuffers(ID3D12GraphicsCommandList* self,
        UINT start, UINT num, const D3D12_VERTEX_BUFFER_VIEW* views) {
    const CommandListVtableHook* e = command_list_hook_entry(self);
    if (!e || !e->iavb_original) return;
    if (CyberpunkVR_SightAxisProbe && views && num >= 1 && start == 7) {
        t_inst_va = views[0].BufferLocation;
        t_inst_stride = views[0].StrideInBytes;
    }
    e->iavb_original(self, start, num, views);
}

// Decode the sight's world orientation for this view and print it once. The three rows are the
// instance rotation; the optical axis is the one the vertex shader picks -- the thin bounding-box
// axis -- so printing all three lets the two views be compared directly.
static ID3D12Resource* g_axis_rb[2] = {nullptr, nullptr};
static void* g_axis_map[2] = {nullptr, nullptr};
static std::atomic<int> g_axis_state[2] = {};       // 0 idle, 1 copy recorded, 2 reported
static uint64_t g_axis_when[2] = {0, 0};
static UINT g_axis_inst[2] = {0, 0};

// Print whatever landed in the readback, once the copy has surely retired.
static void sight_axis_drain() {
    for (int v = 0; v < 2; ++v) {
        if (g_axis_state[v].load(std::memory_order_acquire) != 1) continue;
        if (GetTickCount64() - g_axis_when[v] < 300) continue;   // a few frames is plenty
        g_axis_state[v].store(2, std::memory_order_release);
        const float* r = static_cast<const float*>(g_axis_map[v]);
        if (!r) continue;
        // THE PART THAT WAS NEVER READ. Rows 0..2 gave the rotation, which came out identical
        // in both views; the .w of each row is the instance's TRANSLATION, int32 fixed point at
        // 1/131072, rebased per view. And the rebase cancels: the rebase origin IS the view's
        // camera position (measured: 417378622/131072 = 3184.34617 = _25_m0[37].x), and
        // wp = _25_m0[37] + (instW - rebase)/131072, so wp = instW/131072 outright. One divide
        // and we have the sight's WORLD position for each view, with no constant buffer needed.
        //
        // This is the fork. Identical between views => the weapon is placed once in the world,
        // the sight axis passes through ONE eye, and the other is an IPD off it. Differing by
        // the eye separation => the weapon is placed per view, both eyes sit on their own axis,
        // and the zero distance can have no effect -- which is what was observed.
        const uint32_t* u = static_cast<const uint32_t*>(g_axis_map[v]);
        const double kFp = 1.0 / 131072.0;
        log("[sightaxis] %-5s inst=%u  row0=(%+.6f %+.6f %+.6f)  row1=(%+.6f %+.6f %+.6f)  "
            "row2=(%+.6f %+.6f %+.6f)  world=(%.5f %.5f %.5f)",
            v ? "VRCAM" : "MAIN", g_axis_inst[v],
            r[0], r[1], r[2], r[4], r[5], r[6], r[8], r[9], r[10],
            static_cast<int32_t>(u[3]) * kFp,
            static_cast<int32_t>(u[7]) * kFp,
            static_cast<int32_t>(u[11]) * kFp);
    }
}

// Re-arm both slots at once, every few seconds. Sampling the two views in DIFFERENT frames
// would let the weapon's own sway (centimetres) drown the 65 mm we are looking for; re-arming
// them together keeps each pair one frame apart at worst, and repeating it makes a one-off
// coincidence visible as noise instead of being mistaken for the answer.
extern "C" __declspec(dllexport) int32_t CyberpunkVR_SightAxisRepeatMs = 4000;

static void sight_axis_readback(ID3D12GraphicsCommandList* list, bool vrcam, UINT sinst,
                                uint64_t va) {
    const int v = vrcam ? 1 : 0;
    if (CyberpunkVR_SightAxisRepeatMs > 0 && !vrcam) {
        static uint64_t s_armed = 0;
        const uint64_t now = GetTickCount64();
        if (g_axis_state[0].load(std::memory_order_acquire) == 2 &&
            g_axis_state[1].load(std::memory_order_acquire) == 2 &&
            now - s_armed > static_cast<uint64_t>(CyberpunkVR_SightAxisRepeatMs)) {
            s_armed = now;
            g_axis_state[0].store(0, std::memory_order_release);
            g_axis_state[1].store(0, std::memory_order_release);
        }
    }
    if (g_axis_state[v].load(std::memory_order_acquire) != 0) return;
    const CommandListVtableHook* e = command_list_hook_entry(list);
    if (!e || !e->cbr_original || !g_game_device) return;
    uint64_t off = 0;
    ID3D12Resource* res = buf_for_va(va, 64, &off);
    if (!res) {
        static uint64_t s_last = 0;
        const uint64_t now = GetTickCount64();
        if (!s_last || now - s_last > 10000) {
            s_last = now;
            log("[sightaxis] va=%llX not in any recorded buffer (created before the hook?)",
                (unsigned long long)va);
        }
        return;
    }
    if (!g_axis_rb[v]) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        hp.CreationNodeMask = hp.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 64; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_game_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_axis_rb[v]))) ||
            !g_axis_rb[v]) return;
        if (FAILED(g_axis_rb[v]->Map(0, nullptr, &g_axis_map[v]))) { g_axis_map[v] = nullptr; return; }
    }
    e->cbr_original(list, g_axis_rb[v], 0, res, off, 64);
    g_axis_inst[v] = sinst;
    g_axis_when[v] = GetTickCount64();
    g_axis_state[v].store(1, std::memory_order_release);
}

static void sight_axis_note(bool vrcam, UINT sinst) {
    static bool s_done[2] = {false, false};
    const int v = vrcam ? 1 : 0;
    if (s_done[v] || !t_inst_va || t_inst_stride < 48) return;
    const uint64_t va = t_inst_va + static_cast<uint64_t>(sinst) * t_inst_stride;
    const uint8_t* p = upload_cpu_for_va(va, t_inst_stride);
    if (!p) p = filled_cpu_for_va(va, t_inst_stride);
    if (!p) {
        static uint64_t s_last = 0;
        const uint64_t now = GetTickCount64();
        (void)s_last; (void)now;
        return;                       // the caller falls back to the GPU readback
    }
    float r[12];
    __try { memcpy(r, p, sizeof(r)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    s_done[v] = true;
    log("[sightaxis] %-5s inst=%u  row0=(%+.6f %+.6f %+.6f)  row1=(%+.6f %+.6f %+.6f)  "
        "row2=(%+.6f %+.6f %+.6f)",
        vrcam ? "VRCAM" : "MAIN", sinst,
        r[0], r[1], r[2], r[4], r[5], r[6], r[8], r[9], r[10]);
}

static void STDMETHODCALLTYPE hk_SetPipelineState(ID3D12GraphicsCommandList* self,
        ID3D12PipelineState* pso) {
    const CommandListVtableHook* e = command_list_hook_entry(self);
    if (!e || !e->setpso_original) return;
    t_current_pso = pso;
    e->setpso_original(self, pso);
}

// ---- draw census: which nodes draw for one view and never for the other ---------------------
extern "C" __declspec(dllexport) int32_t CyberpunkVR_DrawCensus = 1;   // OFF: [draw] per-node draw census + imbalance
struct DrawBin { uint32_t node_rva; uint64_t hits[2]; };
static std::array<DrawBin, 96> g_draw{};
static uint32_t g_draw_n = 0;
static std::mutex g_draw_mtx;

static void draw_census_report() {
    static uint64_t s_last = 0;
    const uint64_t now = GetTickCount64();
    if (s_last && now - s_last < 15000) return;
    DrawBin b[96];
    uint32_t n;
    {
        std::lock_guard<std::mutex> lk(g_draw_mtx);
        n = g_draw_n;
        for (uint32_t i = 0; i < n; ++i) b[i] = g_draw[i];
    }
    bool anym = false, anyv = false;
    for (uint32_t i = 0; i < n; ++i) {
        if (b[i].hits[0]) anym = true;
        if (b[i].hits[1]) anyv = true;
    }
    if (!anym || !anyv) return;
    s_last = now;
    for (int pass = 0; pass < 2; ++pass) {
        char line[1100];
        int u = 0, c = 0;
        line[0] = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const uint64_t mine = b[i].hits[pass], other = b[i].hits[pass ^ 1];
            if (!mine || other) continue;
            ++c;
            if (u < static_cast<int>(sizeof(line)) - 28)
                u += snprintf(line + u, sizeof(line) - u, "%X(%llu) ",
                              b[i].node_rva, (unsigned long long)mine);
        }
        log("[draw] nodes that DRAW for %s and never for %s (%d of %u): %s",
            pass ? "VRCAM" : "MAIN", pass ? "MAIN" : "VRCAM", c, n, c ? line : "(none)");
    }
    // Exclusive nodes are only half the story: a node can draw for both views and still do
    // almost nothing for one of them. That is the shape the old audit hinted at for
    // RenderVisionElements (11012 vs 1607), and printing only the exclusive set hides it --
    // the same blind spot that cost this project two wrong turns today in other tools.
    char line[1100];
    int u = 0, c = 0;
    line[0] = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint64_t m = b[i].hits[0], v = b[i].hits[1];
        if (!m || !v) continue;
        const uint64_t hi = m > v ? m : v, lo = m > v ? v : m;
        if (hi < lo * 3) continue;
        ++c;
        if (u < static_cast<int>(sizeof(line)) - 44)
            u += snprintf(line + u, sizeof(line) - u, "%X(M%llu/V%llu %.1fx) ", b[i].node_rva,
                          (unsigned long long)m, (unsigned long long)v,
                          static_cast<double>(hi) / static_cast<double>(lo));
    }
    log("[draw] nodes both views draw but >=3x imbalanced (%d): %s", c, c ? line : "(none)");
}

static void draw_census_note(bool vrcam) {
    if (!g_exe_base) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(g_exe_base);
    const uintptr_t work = t_current_node_work;
    if (work <= base) return;
    const uint32_t rva = static_cast<uint32_t>(work - base);
    {
        std::lock_guard<std::mutex> lk(g_draw_mtx);
        uint32_t i = 0;
        for (; i < g_draw_n; ++i) if (g_draw[i].node_rva == rva) break;
        if (i == g_draw_n) {
            if (g_draw_n >= g_draw.size()) return;
            g_draw[g_draw_n++] = { rva, {0, 0} };
        }
        ++g_draw[i].hits[vrcam ? 1 : 0];
    }
    draw_census_report();
}

static void STDMETHODCALLTYPE hk_DrawInstanced(ID3D12GraphicsCommandList* self,
        UINT vtx, UINT inst, UINT sv, UINT si) {
    const CommandListVtableHook* e = command_list_hook_entry(self);
    if (!e || !e->draw_original) return;
    e->draw_original(self, vtx, inst, sv, si);
    if (CyberpunkVR_DrawCensus) draw_census_note(t_vrcam_node_active);
}

static void STDMETHODCALLTYPE hk_DrawIndexedInstanced(ID3D12GraphicsCommandList* self,
        UINT idx, UINT inst, UINT si, INT bv, UINT sinst) {
    const CommandListVtableHook* e = command_list_hook_entry(self);
    if (!e || !e->drawidx_original) return;
    // The sight's exact draw shape, from the capture: 6 indices, one instance, no index or vertex
    // offset, and an instance slot picked by StartInstanceLocation. Resolved BEFORE the call,
    // because the skip test has to be able to withhold it.
    if (CyberpunkVR_SightAxisProbe && idx == 6 && inst == 1 && si == 0 && bv == 0 && sinst != 0) {
        // Only for the sight's own pixel shader, so the many other 6-index quads cost nothing.
        ID3D12PipelineState* pso = t_current_pso;
        if (pso) {
            uint64_t ps = 0;
            {
                std::lock_guard<std::mutex> lk(g_pso_ids_mtx);
                auto it = g_pso_ids.find(pso);
                if (it != g_pso_ids.end()) ps = it->second.ps;
            }
            if (ps == CyberpunkVR_SightPsHash) {
                sight_axis_note(t_vrcam_node_active, sinst);
                sight_axis_readback(self, t_vrcam_node_active, sinst,
                                    t_inst_va + static_cast<uint64_t>(sinst) * t_inst_stride);
                sight_axis_drain();
            }
        }
    }
    if (CyberpunkVR_PsoProbe && idx == 6 && inst == 1 && si == 0 && bv == 0 && sinst != 0) {
        const uint64_t ps = sight_note(t_vrcam_node_active);
        if (ps && ps == CyberpunkVR_SightSkipPs) return;
    }
    e->drawidx_original(self, idx, inst, si, bv, sinst);
    if (CyberpunkVR_DrawCensus) draw_census_note(t_vrcam_node_active);
}

static void STDMETHODCALLTYPE hk_ExecuteIndirect(ID3D12GraphicsCommandList* self,
        ID3D12CommandSignature* sig, UINT maxCount, ID3D12Resource* args, UINT64 argOff,
        ID3D12Resource* cnt, UINT64 cntOff) {
    const CommandListVtableHook* e = command_list_hook_entry(self);
    if (!e || !e->indirect_original) return;
    e->indirect_original(self, sig, maxCount, args, argOff, cnt, cntOff);
    if (!CyberpunkVR_IndirectCensus || !g_exe_base) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(g_exe_base);
    const uintptr_t work = t_current_node_work;
    if (work <= base) return;
    const uint32_t rva = static_cast<uint32_t>(work - base);
    const int v = t_vrcam_node_active ? 1 : 0;
    {
        std::lock_guard<std::mutex> lk(g_ind_mtx);
        uint32_t i = 0;
        for (; i < g_ind_n; ++i) if (g_ind[i].node_rva == rva && g_ind[i].sig == sig) break;
        if (i == g_ind_n) {
            if (g_ind_n >= g_ind.size()) return;
            g_ind[g_ind_n++] = { rva, sig, {0, 0} };
        }
        ++g_ind[i].hits[v];
    }
    indirect_report();
}

// ---- which node builds the colour-grading tables? -------------------------------------------
// The scanner's green tint is missing in the second eye, and the capture says why: the tonemap
// pass (PipelineState_777) is bound `3 x Texture3D<float3>` -- three 48^3 R11G11B10 tables --
// and at VRCAM's draw they hold a neutral colour cube while at MAIN's draw they hold the graded
// green one. Same shader, same three resources, rebuilt once per view by three 6x6x6 dispatches
// (48^3 / 8^3) in AsyncComputeDuringShadowmaps; only the per-view constants differ, and VRCAM's
// come out ungraded.
//
// The tables are a SHARED resource and the frame order is VRCAM then MAIN, so the fix is simply
// to let VRCAM skip its own build and sample the ones MAIN left. Colour grading is
// view-independent -- both eyes MUST have the same grade -- so that is the correct answer, not a
// workaround, and it costs the second eye one frame of grading latency.
//
// This probe exists only to name the node, so CyberpunkVR_NodeCutSet can target it without
// another rebuild. Cubic thread-group shapes are rare enough to be a clean filter.
extern "C" __declspec(dllexport) int32_t CyberpunkVR_VolumeNodeProbe = 1;   // OFF: cubic-dispatch (volume/LUT) probe
static void volume_node_note(uint32_t rva, UINT n, bool vrcam) {
    struct Seen { uint32_t rva, n; uint8_t views; };
    static std::array<Seen, 16> s_seen{};
    static uint32_t s_n = 0;
    static std::mutex s_mtx;
    char line[600];
    int used = 0;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        uint32_t i = 0;
        for (; i < s_n; ++i) if (s_seen[i].rva == rva && s_seen[i].n == n) break;
        const uint8_t bit = vrcam ? 2 : 1;
        if (i < s_n) {
            if (s_seen[i].views & bit) return;          // already reported for this view
            s_seen[i].views |= bit;
        } else {
            if (s_n >= s_seen.size()) return;
            s_seen[s_n++] = { rva, n, bit };
        }
        line[0] = 0;
        for (uint32_t k = 0; k < s_n && used < static_cast<int>(sizeof(line)) - 40; ++k)
            used += snprintf(line + used, sizeof(line) - used, "%X:%ux%ux%u(%s%s) ",
                             s_seen[k].rva, s_seen[k].n, s_seen[k].n, s_seen[k].n,
                             (s_seen[k].views & 1) ? "M" : "-",
                             (s_seen[k].views & 2) ? "V" : "-");
    }
    log("[volnode] cubic dispatches, node:shape(views): %s", line);
}

static void STDMETHODCALLTYPE hk_Dispatch(ID3D12GraphicsCommandList* self,
        UINT x, UINT y, UINT z) {
    const CommandListVtableHook* e = command_list_hook_entry(self);
    if (!e || !e->dispatch_original) return;
    e->dispatch_original(self, x, y, z);
    t_last_disp[0] = x; t_last_disp[1] = y; t_last_disp[2] = z;
    if (CyberpunkVR_DispatchCensus && g_exe_base) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(g_exe_base);
        const uintptr_t work = t_current_node_work;
        if (work > base) {
            dispatch_census_note(static_cast<uint32_t>(work - base), x, y, z,
                                 t_vrcam_node_active);
            dispatch_census_report();
        }
    }
    if (CyberpunkVR_VolumeNodeProbe && g_exe_base && x == y && y == z && x >= 2 && x <= 16) {
        // Record UNATTRIBUTED ones too (rva 0). The 6x6x6 grading-volume builds live in the
        // AsyncComputeDuringShadowmaps list and never showed up here, and the question that
        // decides the fix is WHY: if they arrive with rva 0 then t_current_node_work -- which is
        // thread-local and set by Detour_NodeDispatch -- is simply not set on whatever thread
        // records that list, and no node-level hook can ever see them.
        const uintptr_t base = reinterpret_cast<uintptr_t>(g_exe_base);
        const uintptr_t work = t_current_node_work;
        volume_node_note(work > base ? static_cast<uint32_t>(work - base) : 0u, x,
                         t_vrcam_node_active);
    }
    if (CyberpunkVR_LightContent) light_content_report();
}

static void STDMETHODCALLTYPE hk_CopyBufferRegion(ID3D12GraphicsCommandList* self,
        ID3D12Resource* dst, UINT64 dst_off, ID3D12Resource* src, UINT64 src_off,
        UINT64 num_bytes) {
    const CommandListVtableHook* e = command_list_hook_entry(self);
    if (!e || !e->cbr_original) return;
    e->cbr_original(self, dst, dst_off, src, src_off, num_bytes);
    // ---- how many lights does each view actually get? ---------------------------------------
    // Everything measurable about the two views is identical -- same camera to the byte bar the
    // 6.4 cm IPD, same near/far, same FOV, same passes, same cull constants -- and yet a whole
    // class of lights never lights in VRCAM at any distance. So stop comparing inputs and
    // measure the OUTPUT: the light nodes upload their per-view light array through here, and
    // its byte volume is proportional to the number of lights that survived collection.
    // Fewer bytes for VRCAM = lights are lost during collection; equal bytes = they are all
    // there and the difference is downstream, in how they are applied.
    // Sizes and counts of the light uploads match bin for bin, and every lighting node now
    // provably dispatches for both views -- so the remaining difference can only be in the
    // CONTENT of the arrays. Snapshot the largest upload each view makes inside the light nodes
    // and compare them: same length and near-identical bytes means the lights really are the
    // same and the defect is in shading; a truncation or a run of zeros in VRCAM's is the answer.
    if (CyberpunkVR_LightContent && src && num_bytes >= 4096 && num_bytes <= LIGHT_SNAP_MAX &&
            g_exe_base) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(g_exe_base);
        const uintptr_t work = t_current_node_work;
        if (work > base) {
            const uint32_t rva = static_cast<uint32_t>(work - base);
            if (rva == CLUSTERED_LIGHTS_CULL_RVA || rva == RENDER_LIGHT_BUFFERS_RVA)
                light_content_note(src, src_off, num_bytes, t_vrcam_node_active);
        }
    }
    if (CyberpunkVR_LightCensus && g_exe_base && num_bytes) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(g_exe_base);
        const uintptr_t work = t_current_node_work;
        if (work > base) {
            const uint32_t rva = static_cast<uint32_t>(work - base);
            if (rva == CLUSTERED_LIGHTS_CULL_RVA || rva == RENDER_LIGHT_BUFFERS_RVA) {
                volatile LONG64* bytes = t_vrcam_node_active
                    ? reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugLightBytesVrcam)
                    : reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugLightBytesMain);
                volatile LONG64* count = t_vrcam_node_active
                    ? reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugLightUploadsVrcam)
                    : reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugLightUploadsMain);
                InterlockedExchangeAdd64(bytes, static_cast<LONG64>(num_bytes));
                InterlockedIncrement64(count);
                D3D12_RESOURCE_DESC dd{};
                const uint64_t dst_size =
                    (dst && mirror_get_resource_desc(dst, &dd)) ? dd.Width : 0;
                light_census_note(num_bytes, dst_size, dst, t_vrcam_node_active);
                light_census_report();
            }
        }
    }
    // First 848B CB upload after the vrcam tonemap 2-RT bind = the pass's constants
    // (observed: bind ev95006 -> upload ev95009, one per window).
    if (t_in_vrcam_2rt && t_2rt_cb_armed && dst && num_bytes == 848) {
        t_2rt_cb_armed = false;
        ID3D12Resource* prev = g_cb_res.exchange(dst, std::memory_order_acq_rel);
        if (prev != dst) {
            dst->AddRef();
            if (prev) prev->Release();
        }
        g_cb_off.store(dst_off, std::memory_order_release);
        InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(
            &CyberpunkVR_DebugCbCaptures));
    }
    // Every fill from a mappable source, remembered so a DEFAULT-heap buffer can still be read on
    // the CPU. Gated: it costs one GetGPUVirtualAddress per copy.
    if (CyberpunkVR_SightAxisProbe && dst && src && num_bytes >= 64) {
        if (const uint8_t* sp = upload_map_read(src))
            filled_note_guarded(dst, dst_off, num_bytes, sp + src_off);
    }
    // The per-frame constants the HUD composite reads as b0 -- 30 float4, and its first float is
    // the time that drives the HUD's scanline flicker. Exactly one 480-byte constant upload
    // happens per frame, which is what identifies it. Captured so our port can bind the SAME
    // buffer as a root CBV and read the SAME value: the flicker is a spatial pattern, so a phase
    // of our own would make the two eyes disagree pixel by pixel.
    if (dst && num_bytes == 480 && dst_off == 0) {
        ID3D12Resource* prev = g_frame_cb.load(std::memory_order_acquire);
        if (prev != dst) {
            dst->AddRef();
            g_frame_cb.store(dst, std::memory_order_release);
            if (prev) prev->Release();
            CyberpunkVR_DebugFrameCb = reinterpret_cast<uint64_t>(dst);
        }
    }
    // The composite's OWN constants (b6). Identified by CONTENT rather than by size: the capture
    // showed a single 512-byte upload, but live at 2560x2560 there is none, and keying on the
    // size simply never matched. The upload heap is CPU-visible, so the source bytes can be read
    // here and checked -- register 16 zw is the composite's target size, which nothing else
    // carries. That also makes the binding correct at any resolution, which is the whole point:
    // the constants read out of a 1920x1080 capture are not the ones a 2560 square uses.
    if (dst && src && num_bytes >= 272 && num_bytes <= 4096 &&
        !g_hud_cb_from_ring.load(std::memory_order_acquire)) {
        uint64_t w = 0, h = 0;
        {
            std::lock_guard<std::mutex> lk(g_hud_snap_mtx);
            w = g_hud_snap_desc.Width; h = g_hud_snap_desc.Height;
        }
        float curve[2] = {0.0f, 0.0f};
        if (w && h && hud_cb_content_matches(upload_map_read(src), src_off,
                                             (float)w, (float)h, curve)) {
            ID3D12Resource* prev = g_hud_cb.load(std::memory_order_acquire);
            if (prev != dst) {
                dst->AddRef();
                g_hud_cb.store(dst, std::memory_order_release);
                if (prev) prev->Release();
                CyberpunkVR_DebugHudCb = reinterpret_cast<uint64_t>(dst);
                log("[hud] composite constants captured: dst=%p bytes=%llu target=%llux%llu "
                    "curvature=(%.6f, %.6f)", dst, (unsigned long long)num_bytes,
                    (unsigned long long)w, (unsigned long long)h, curve[0], curve[1]);
            }
        }
    }
}


static LRESULT CALLBACK MirrorWndProc(HWND hh, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_CLOSE) return 0;               // don't destroy on close
    return DefWindowProcW(hh, m, wp, lp);
}

// ---- D3D12 present thread (real second swapchain for VRCAM) ----------------
// Owns its OWN command queue + D3D12 swapchain + window (message pump, so DWM
// keeps drawing it). Copies g_d12_mtex (filled by the game's appended copy) into
// the backbuffer on its own queue (GPU-waits the game fence -> reads the fresh
// frame, no CPU stall on the game thread) and Presents. Present + window are
// fully off the game render thread -> no FPS drop, smooth input.
static void d12_present_thread() {
    UINT w = 0, h = 0;
    for (int i = 0; i < 6000 && CyberpunkVR_MirrorOutput; ++i) {
        if (g_d12_mtex && g_game_device && g_d12_fence) { w = g_d12_w; h = g_d12_h; break; }
        Sleep(10);
    }
    if (!g_d12_mtex || !g_game_device || !g_d12_fence) return;
    ID3D12Device* dev = g_game_device;

    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q = nullptr;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) || !q) return;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) return;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)))) return;
    list->Close();
    ID3D12Fence* pf = nullptr;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pf)))) return;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr); UINT64 pv = 0;

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = MirrorWndProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CyberpunkVR_Mirror"; RegisterClassExW(&wc);
    // client area == vrcam resolution (w x h) so OBS captures native res, no scaling.
    RECT wr = { 0, 0, (LONG)w, (LONG)h };
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"VRCAM Mirror (OBS)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll"); if (!dxgi) dxgi = LoadLibraryW(L"dxgi.dll");
    using PFN_CDXGI2 = HRESULT (WINAPI*)(UINT, REFIID, void**);
    auto pFac = dxgi ? reinterpret_cast<PFN_CDXGI2>(GetProcAddress(dxgi, "CreateDXGIFactory2")) : nullptr;
    IDXGIFactory2* fac = nullptr;
    if (!pFac || FAILED(pFac(0, IID_PPV_ARGS(&fac))) || !fac) return;
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = w; sd.Height = h; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; sd.Scaling = DXGI_SCALING_STRETCH;
    IDXGISwapChain1* sc1 = nullptr;
    HRESULT hr = fac->CreateSwapChainForHwnd(q, hwnd, &sd, nullptr, nullptr, &sc1);
    fac->Release();
    CyberpunkVR_DebugMirrorLastHr = (uint32_t)hr;
    if (FAILED(hr) || !sc1) return;
    IDXGISwapChain3* sc = nullptr;
    if (FAILED(sc1->QueryInterface(IID_PPV_ARGS(&sc)))) { sc1->Release(); return; }
    sc1->Release();
    CyberpunkVR_DebugMirrorState = 3;
    log("[mirror] d12 present-thread ready %ux%u", w, h);

    // RTV descriptor heap for the 2 backbuffers (used only by the red test pattern).
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvH[2] = {};
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 2;
        if (SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap))) && rtvHeap) {
            const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            const D3D12_CPU_DESCRIPTOR_HANDLE base = rtvHeap->GetCPUDescriptorHandleForHeapStart();
            for (UINT i = 0; i < 2; ++i) {
                rtvH[i] = base; rtvH[i].ptr += (SIZE_T)inc * i;
                ID3D12Resource* bbi = nullptr;
                if (SUCCEEDED(sc->GetBuffer(i, IID_PPV_ARGS(&bbi))) && bbi) {
                    if (g_orig_CreateRTV) g_orig_CreateRTV(dev, bbi, nullptr, rtvH[i]);
                    else dev->CreateRenderTargetView(bbi, nullptr, rtvH[i]);
                    bbi->Release();
                }
            }
        }
    }

    // ---- Format-converting present pipeline -------------------------------
    // The vrcam final (g_d12_mtex) is an HDR packed-float target (R11G11B10),
    // not copy-compatible with the 8-bit backbuffer. Sample it in a fullscreen
    // pass and write the backbuffer -> works for ANY source format (this is the
    // HDR->8bit step the engine's missing swapchain-composition passes would do).
    ID3D12RootSignature* convRoot = nullptr;
    ID3D12PipelineState* convPso  = nullptr;
    ID3D12DescriptorHeap* srvHeap = nullptr;
    bool convOk = false;
    {
        HMODULE d3dc = GetModuleHandleW(L"d3dcompiler_47.dll");
        if (!d3dc) d3dc = LoadLibraryW(L"d3dcompiler_47.dll");
        using PFN_D3DCompile = HRESULT (WINAPI*)(LPCVOID, SIZE_T, LPCSTR,
            const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
            ID3DBlob**, ID3DBlob**);
        auto pCompile = d3dc ? reinterpret_cast<PFN_D3DCompile>(
            GetProcAddress(d3dc, "D3DCompile")) : nullptr;
        HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
        using PFN_SerRS = HRESULT (WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*,
            D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
        auto pSerRS = d3d12 ? reinterpret_cast<PFN_SerRS>(
            GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
        // EXACT replica of the engine's PipelineState_1335 MODE 0 (SDR 8-bit path):
        // Load the HDR texel (no filter) -> max(0) -> sRGB OETF -> animated triangular
        // dither (anti-banding). No tonemap (done earlier by ApplyBloomAndTonemapping),
        // no exposure scale (MODE 0 has none). Matches main's final swapchain write.
        static const char* kHLSL =
            "Texture2D gTex:register(t0);"
            "cbuffer T:register(b0){float gT;};"                          // per-frame time (dither anim)
            "void VSMain(uint vid:SV_VertexID,out float4 pos:SV_Position,out float2 uv:TEXCOORD0){"
            "uv=float2((vid<<1)&2,vid&2);pos=float4(uv.x*2-1,1-uv.y*2,0,1);}"
            "float4 PSMain(float4 pos:SV_Position,float2 uv:TEXCOORD0):SV_Target{"
            "float3 c=max(0.0,gTex.Load(int3(int2(pos.xy),0)).rgb);"
            "float3 st=step(0.0031308,c);"
            "float3 e=lerp(c*12.92,1.055*pow(abs(c),1.0/2.4)-0.055,st);"  // engine sRGB OETF
            "float t=frac(gT*6.2272);float2 p=pos.xy;"                    // triangular dither (animated)
            "float ax=frac(p.x*211.1488),ay=frac(p.y*210.944);"
            "float d1=dot(float3(ax,ay,t),float3(ay+33.33,ax+33.33,t+33.33));"
            "float u1=d1+ax,v1=d1+ay,w1=u1+v1;"
            "float3 n1=float3(frac(w1*(d1+t)),frac((u1*2)*v1),frac(w1*u1));"
            "float2 q=p+64.0;"
            "float bx=frac(q.x*211.1488),by=frac(q.y*210.944);"
            "float d2=dot(float3(bx,by,t),float3(by+33.33,bx+33.33,t+33.33));"
            "float u2=d2+bx,v2=d2+by,w2=u2+v2;"
            "float3 n2=float3(frac(w2*(d2+t)),frac((u2*2)*v2),frac(w2*u2));"
            "float3 vv=e*510.0;float3 edge=min(min(float3(1,1,1),vv),510.0-vv);"
            "float3 o=((n1-0.5)+edge*(n2-0.5))*(1.0/255.0)+e;"
            "return float4(o,1);}";
        ID3DBlob* vs = nullptr; ID3DBlob* ps = nullptr; ID3DBlob* err = nullptr;
        if (pCompile && pSerRS &&
            SUCCEEDED(pCompile(kHLSL, strlen(kHLSL), "conv", nullptr, nullptr,
                "VSMain", "vs_5_0", 0, 0, &vs, &err)) && vs &&
            SUCCEEDED(pCompile(kHLSL, strlen(kHLSL), "conv", nullptr, nullptr,
                "PSMain", "ps_5_0", 0, 0, &ps, &err)) && ps) {
            D3D12_DESCRIPTOR_RANGE range = {};
            range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            range.NumDescriptors = 1;
            D3D12_ROOT_PARAMETER rp[2] = {};
            rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp[0].DescriptorTable.NumDescriptorRanges = 1;
            rp[0].DescriptorTable.pDescriptorRanges = &range;
            rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            rp[1].Constants.ShaderRegister = 0;   // b0
            rp[1].Constants.RegisterSpace = 0;
            rp[1].Constants.Num32BitValues = 1;   // gT
            rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_STATIC_SAMPLER_DESC ss = {};
            ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            ss.MaxLOD = D3D12_FLOAT32_MAX;
            ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_ROOT_SIGNATURE_DESC rsd = {};
            rsd.NumParameters = 2; rsd.pParameters = rp;
            rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &ss;
            ID3DBlob* rsBlob = nullptr;
            if (SUCCEEDED(pSerRS(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, nullptr)) && rsBlob &&
                SUCCEEDED(dev->CreateRootSignature(0, rsBlob->GetBufferPointer(),
                    rsBlob->GetBufferSize(), IID_PPV_ARGS(&convRoot))) && convRoot) {
                D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
                pd.pRootSignature = convRoot;
                pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
                pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
                pd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0xF;
                pd.SampleMask = 0xFFFFFFFF;
                pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                pd.NumRenderTargets = 1;
                pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                pd.SampleDesc.Count = 1;
                if (SUCCEEDED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&convPso))) && convPso) {
                    D3D12_DESCRIPTOR_HEAP_DESC shd = {};
                    shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                    shd.NumDescriptors = 1;
                    shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                    if (SUCCEEDED(dev->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&srvHeap))) && srvHeap) {
                        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
                        sv.Format = g_d12_fmt;
                        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        sv.Texture2D.MipLevels = 1;
                        dev->CreateShaderResourceView(g_d12_mtex, &sv,
                            srvHeap->GetCPUDescriptorHandleForHeapStart());
                        convOk = true;
                    }
                }
            }
            if (rsBlob) rsBlob->Release();
        }
        if (vs) vs->Release(); if (ps) ps->Release(); if (err) err->Release();
        log("[mirror] convert-pipeline %s (mtex fmt=%u)",
            convOk ? "ready" : "FAILED", (unsigned)g_d12_fmt);
    }

    uint64_t last = 0;
    bool shown = true;                       // window was created visible
    for (;;) {
        MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        // Follow the toggle with the window itself: pausing Present alone would leave a frozen
        // mirror window on screen, which reads as "the toggle did nothing".
        const bool want = CyberpunkVR_MirrorOutput != 0;
        if (want != shown) {
            shown = want;
            ShowWindow(hwnd, want ? SW_SHOWNOACTIVATE : SW_HIDE);
        }
        if (!want) { Sleep(50); continue; }
        const bool testpat = (CyberpunkVR_MirrorTestPattern != 0) && rtvHeap;
        const uint64_t rdy = g_d12_ready.load(std::memory_order_acquire);
        if (!testpat && rdy <= last) { Sleep(2); continue; }
        if (!testpat) q->Wait(g_d12_fence, rdy);   // GPU: our copy after the game's write
        alloc->Reset(); list->Reset(alloc, nullptr);
        const UINT idx = sc->GetCurrentBackBufferIndex();
        ID3D12Resource* bb = nullptr;
        if (SUCCEEDED(sc->GetBuffer(idx, IID_PPV_ARGS(&bb))) && bb) {
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = bb;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            if (testpat) {
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                b.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
                list->ResourceBarrier(1, &b);
                const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
                list->ClearRenderTargetView(rtvH[idx], red, 0, nullptr);
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
                list->ResourceBarrier(1, &b);
            } else if (convOk && rtvHeap) {
                // HDR (e.g. R11G11B10) mtex -> 8-bit backbuffer via a fullscreen
                // sample+write -- the same HDR->8bit composite MAIN's RenderFinal2D
                // does into its swapchain backbuffer, but for the vrcam final.
                D3D12_RESOURCE_BARRIER mb = {};
                mb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                mb.Transition.pResource = g_d12_mtex;
                mb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                mb.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                mb.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                list->ResourceBarrier(1, &mb);
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                b.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
                list->ResourceBarrier(1, &b);
                ID3D12DescriptorHeap* heaps[] = { srvHeap };
                list->SetDescriptorHeaps(1, heaps);
                list->SetGraphicsRootSignature(convRoot);
                list->SetPipelineState(convPso);
                list->SetGraphicsRootDescriptorTable(0,
                    srvHeap->GetGPUDescriptorHandleForHeapStart());
                float ditherT = (float)(pv & 0x3FF);   // animate dither per present frame
                list->SetGraphicsRoot32BitConstants(1, 1, &ditherT, 0);
                D3D12_VIEWPORT vp = { 0.f, 0.f, (float)w, (float)h, 0.f, 1.f };
                D3D12_RECT rc = { 0, 0, (LONG)w, (LONG)h };
                list->RSSetViewports(1, &vp);
                list->RSSetScissorRects(1, &rc);
                list->OMSetRenderTargets(1, &rtvH[idx], FALSE, nullptr);
                list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                list->DrawInstanced(3, 1, 0, 0);
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
                list->ResourceBarrier(1, &b);
                mb.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                mb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
                list->ResourceBarrier(1, &mb);
            } else {
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
                list->ResourceBarrier(1, &b);
                list->CopyResource(bb, g_d12_mtex);    // mtex COMMON -> implicit COPY_SOURCE
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
                list->ResourceBarrier(1, &b);
            }
            list->Close();
            ID3D12CommandList* ls[] = { list };
            q->ExecuteCommandLists(1, ls);
            sc->Present(testpat ? 0 : 1, 0);       // vsync on OUR thread only
            bb->Release();
            q->Signal(pf, ++pv);                   // throttle THIS thread (allocator reuse)
            if (pf->GetCompletedValue() < pv) { pf->SetEventOnCompletion(pv, ev); WaitForSingleObject(ev, 100); }
            ++CyberpunkVR_DebugMirrorFrames;
            last = rdy;
            if (testpat) Sleep(16);
        } else {
            list->Close();
        }
    }
}


// Resolve the view-output ctx from the node arg via the engine's own vcall
// (obj = ([[a2]] vtable[+0x20])()), returning &obj[0xF94] (AA/upscaler mode).
static uintptr_t sl_view_obj(void* a2) {
    uintptr_t X = *reinterpret_cast<uintptr_t*>(a2);            // *(a2)
    if (!X) return 0;
    uintptr_t vt = *reinterpret_cast<uintptr_t*>(X);           // X's vtable
    using GetViewFn = uintptr_t(__fastcall*)(uintptr_t);
    GetViewFn getv = *reinterpret_cast<GetViewFn*>(vt + 0x20);
    return getv(X);                                            // view-state object
}

static __int64 __fastcall Detour_SlConstants(void* a1, void* a2, void* a3) {
    // MAIN identity, step 2: this writer is the only place that sees the view OBJECT and the
    // view CTX for the same view, so it is where the object recorded from MAIN-only nodes
    // becomes a ctx pointer every other hook can compare against. Re-pinned every frame, so a
    // ctx pool that recycles pointers cannot leave a stale MAIN behind for more than a frame.
    // key == 0 is REQUIRED, not a nicety: without it this pinned VRCAM's ctx as MAIN (the
    // object the work-context vtable hands back is not per-view enough to separate them),
    // after which is_main_view() answered true for VRCAM and its whole branch went dead.
    if (a2) {
        __try {
            const uintptr_t want = g_main_view_obj.load(std::memory_order_acquire);
            if (want) {
                const uintptr_t ctx = *reinterpret_cast<uintptr_t*>(
                    reinterpret_cast<uint8_t*>(a2) + 0x18);
                if (ctx && *reinterpret_cast<uint64_t*>(ctx + 0x28) == 0 &&
                    sl_view_obj(a2) == want &&
                    g_main_view_ctx.exchange(ctx, std::memory_order_release) != ctx) {
                    ++CyberpunkVR_DebugMainCtxBinds;
                    CyberpunkVR_DebugMainCtx = static_cast<uint64_t>(ctx);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (CyberpunkVR_StreamlineHistoryFix && a2) {
        __try {
            uintptr_t ctx = *reinterpret_cast<uintptr_t*>(
                reinterpret_cast<uint8_t*>(a2) + 0x18);
            if (ctx) {
                uint64_t key = *reinterpret_cast<uint64_t*>(ctx + 0x28);
                uintptr_t obj = sl_view_obj(a2);
                if (obj) {
                    uint32_t* mode = reinterpret_cast<uint32_t*>(obj + 0xF94);
                    const uint32_t build_mode =
                        *reinterpret_cast<uint32_t*>(obj + 0xF90);
                    if (key == g_vrcam_ctx_key) {
                        CyberpunkVR_DebugVrcamAaMode = *mode;
                        CyberpunkVR_DebugVrcamBuildModeF90 = build_mode;
                        uint32_t want = CyberpunkVR_DebugMainAaMode;   // mirror main
                        if (*mode != want) { *mode = want; ++CyberpunkVR_DebugSlHistoryHits; }
                    } else if (key == 0) {
                        CyberpunkVR_DebugMainAaMode = *mode;           // observe main
                        CyberpunkVR_DebugMainBuildModeF90 = build_mode;
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // ---- per-eye IPD stereo + tripod + vrcam mirror (all independent, default OFF) ----
    // choice A: main = LEFT (-IPD/2), vrcam = RIGHT (+IPD/2). Applied BEFORE g_orig and
    // NOT restored so the whole frame (incl. the once-per-frame prev-camera capture) sees
    // the same camera -> motion vectors stay consistent (no shimmer).
    // Force VRCAM's camera scalars to match MAIN via the camera context (this writer runs
    // many times per frame, exactly like the IPD transform above). Capture MAIN (slot 0),
    // apply to VRCAM (slot 1): FOV@0x90, camera zoom@0x9C, near@0xB0 and far@0xB4.
    // Orientation (@0x80/0xC0) is left to the engine; aspect stays VRCAM's own.
    if (CyberpunkVR_ForceVrcamCam && a2) {
        __try {
            uintptr_t ctx = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(a2) + 0x18);
            if (ctx) {
                const uint64_t key = *reinterpret_cast<uint64_t*>(ctx + 0x28);
                float* c = reinterpret_cast<float*>(ctx);
                if (is_main_view(reinterpret_cast<void*>(ctx))) {
                    g_main_cam_fov  = c[0x90 / 4];
                    g_main_cam_zoom = c[0x9C / 4];
                    g_main_cam_near = c[0xB0 / 4];
                    g_main_cam_far  = c[0xB4 / 4];
                    g_main_proj_yy  = c[0x214 / 4];
                    // Effective MAIN/world ADS magnification as a plain scale factor. MAIN's
                    // FOV scalar is provably NOT
                    // touched by ADS (measured: 68.238 both at rest and while aiming), so it
                    // still describes the UNZOOMED frustum -- which makes cot(fovV/2) the
                    // baseline the live vertical scale divides by. No guessed reference.
                    // Measured: at rest 1.47593 * tan(34.119deg) = 1.0000; aiming 2.21332 the
                    // same way = 1.4998.
                    if (g_main_cam_fov > 0.f) {
                        const float t = tanf(g_main_cam_fov * 0.5f * 0.01745329252f);
                        if (t > 0.f) {
                            g_main_ads_zoom_factor = g_main_proj_yy * t;
                            CyberpunkVR_MainAdsZoomFactor = g_main_ads_zoom_factor;
                        }
                    }
                    CyberpunkVR_DebugMainCamFov = g_main_cam_fov;
                    CyberpunkVR_DebugMainProjYY = g_main_proj_yy;
                } else if (key == g_vrcam_ctx_key && g_main_cam_fov > 0.f) {
                    // ctx scalars: drive vrcam culling/LOD to match main (screen-space).
                    c[0x90 / 4] = g_main_cam_fov;        // fov
                    c[0x9C / 4] = g_main_cam_zoom;       // zoom
                    c[0xB0 / 4] = g_main_cam_near;       // near
                    c[0xB4 / 4] = g_main_cam_far;        // far

                    // ---- VRCAM vertical FOV: the only input the RTT projection has --------
                    // Established by measurement (engine_re/dumps/F_rtt_camera_fov.md,
                    // G_rtt_zoom_consumer*.md, H_rtt_zoom_field.md):
                    //   * the projection is built from the component's fov at comp+0x128 and
                    //     nothing else -- the producer's source struct holds fov/aspect/
                    //     near/far and cot(68.238/2) == 1.47593 reproduces it exactly;
                    //   * the RTTI `zoom` field at +0x15C is never read on this path;
                    //   * the zoom ratio at +0x424 is an OUTPUT of the per-view setup;
                    //   * writing the projection into the view ctx steers CULLING only;
                    //   * that producer runs EVERY frame, standing still included, so the fov
                    //     write is sufficient on its own. Nothing needs re-invoking -- forcing
                    //     comp+0xA00 or calling sub_140AC316C drags view-create in, which
                    //     hitched the game and hung the GPU (DXGI_ERROR_DEVICE_HUNG).
                    // Computed in double: the value round-trips through the engine as
                    // fov -> cot -> projection, and doing the trig in float left ~0.002 deg
                    // of drift against the authored value.
                    double src_fov = 0.0;              // vertical FOV in degrees, pre-ADS
                    double ads = static_cast<double>(g_main_ads_zoom_factor);
                    if (CyberpunkVR_VrcamFovDeg > 1.0f) {
                        // Explicit override -- this is where the headset's own FOV goes once
                        // the HMD drives the eye. ADS still applies on top of it.
                        src_fov = static_cast<double>(CyberpunkVR_VrcamFovDeg);
                    } else if (g_main_proj_yy > 0.f) {
                        // Follow MAIN. Its projection ALREADY carries the ADS zoom, so the
                        // factor must not be applied a second time.
                        src_fov = 2.0 * atan(1.0 / static_cast<double>(g_main_proj_yy)) *
                                  57.29577951308232;
                        ads = 1.0;
                    } else if (g_vrcam_base_fov > 0.f) {
                        src_fov = static_cast<double>(g_vrcam_base_fov);
                    }
                    const uintptr_t comp = g_vrcam_comp.load(std::memory_order_acquire);
                    if (comp && src_fov > 1.0 && ads > 0.0) {
                        const double want = (ads == 1.0)
                            ? src_fov
                            : 2.0 * atan(tan(src_fov * 0.5 * 0.017453292519943295) / ads) *
                              57.29577951308232;
                        if (want > 1.0 && want < 175.0) {
                            *reinterpret_cast<float*>(comp + 0x128) = static_cast<float>(want);
                            CyberpunkVR_DebugVrcamWantFov = static_cast<float>(want);
                        }
                    }
                    ++CyberpunkVR_DebugForceCamHits;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // DLSS-for-vrcam: mark this camera-writer call as vrcam so the constants driver
    // (sub_14078933C, called INSIDE g_orig) flips to vrcam's own SL viewport.
    const bool prev_sl_active = t_vrcam_sl_active;
    if (CyberpunkVR_VrcamDlss && a2) {
        __try {
            uintptr_t ctx = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(a2) + 0x18);
            t_vrcam_sl_active = ctx && *reinterpret_cast<uint64_t*>(ctx + 0x28) == g_vrcam_ctx_key;
        } __except (EXCEPTION_EXECUTE_HANDLER) { t_vrcam_sl_active = false; }
    }
    __int64 sl_ret = g_orig_sl_const(a1, a2, a3);
    t_vrcam_sl_active = prev_sl_active;
    return sl_ret;
}

// DLSS constants driver (sub_14078933C -> slSetConstants). Flip to vrcam's SL
// viewport while the camera writer is processing the vrcam view.
static __int64 __fastcall Detour_DlssConst(void* a1, unsigned int a2) {
    bool flipped = false; int32_t saved = 0;
    if (CyberpunkVR_VrcamDlss && t_vrcam_sl_active && a1) {
        __try {
            int32_t* vp = reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(a1) + DLSS_VP_OFF);
            saved = *vp; *vp = CyberpunkVR_VrcamDlssViewport; flipped = true;
            // A/B: zero the jitter the const-setter (g_orig) is about to copy into sl::Constants.
            // The camera-writer filled it just before this call; zeroing here makes DLSS treat the
            // vrcam frame as un-jittered.
            if (CyberpunkVR_VrcamDlssZeroJitter) {
                *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(a1) + DLSS_JITTER_OFF)     = 0.0f;
                *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(a1) + DLSS_JITTER_OFF + 4) = 0.0f;
            }
            ++CyberpunkVR_DebugVrcamDlssConstHits;
        } __except (EXCEPTION_EXECUTE_HANDLER) { flipped = false; }
    }
    __int64 r = g_orig_dlss_const(a1, a2);
    if (flipped) {
        __try {
            *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(a1) + DLSS_VP_OFF) = saved;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return r;
}

// DLSS tag+eval driver (sub_141D4FDC0). a2 = view spec; key at *(*(a2+0x18)+0x28).
// Flip to vrcam's own SL viewport around the whole call (covers its internal
// slSetTag x N + slEvaluateFeature) so vrcam gets a distinct DLSS feature/history.
static void __fastcall Detour_DlssEval(void* a1, void* a2, int a3, int a4, int a5,
                                       int a6, int a7, int a8, int a9, int a10,
                                       int a11, int a12, int a13, int a14) {
    bool flipped = false; int32_t saved = 0; uintptr_t cache_addr = 0; uintptr_t vrcam_ctx = 0;
    static uint8_t s_main_cache[DLSS_CACHE_SZ];   // (render thread only; serialized by CS in g_orig)
    if (CyberpunkVR_VrcamDlss && a1 && a2) {
        __try {
            uintptr_t ctx = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(a2) + 0x18);
            if (ctx && *reinterpret_cast<uint64_t*>(ctx + 0x28) == g_vrcam_ctx_key) {
                vrcam_ctx = ctx;
                int32_t* vp = reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(a1) + DLSS_VP_OFF);
                saved = *vp; *vp = CyberpunkVR_VrcamDlssViewport;
                // swap vrcam's own changed-detection cache in (stops per-frame feature recreate)
                cache_addr = reinterpret_cast<uintptr_t>(a1) + DLSS_CACHE_OFF;
                memcpy(s_main_cache, reinterpret_cast<void*>(cache_addr), DLSS_CACHE_SZ);
                if (g_vrcam_dlss_cache_valid)
                    memcpy(reinterpret_cast<void*>(cache_addr), g_vrcam_dlss_cache, DLSS_CACHE_SZ);
                flipped = true;
                ++CyberpunkVR_DebugVrcamDlssEvalHits;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { flipped = false; }
    }
    g_orig_dlss_eval(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    if (flipped) {
        __try {
            // save vrcam's updated cache for next frame, restore main's cache into a1
            memcpy(g_vrcam_dlss_cache, reinterpret_cast<void*>(cache_addr), DLSS_CACHE_SZ);
            g_vrcam_dlss_cache_valid = true;
            memcpy(reinterpret_cast<void*>(cache_addr), s_main_cache, DLSS_CACHE_SZ);
            *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(a1) + DLSS_VP_OFF) = saved;
            (void)vrcam_ctx;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    // POST-DLSS CROP FIX: vrcam's DLSS eval just recorded on THIS command-list thread; mark the
    // thread as post-DLSS so the RSSetViewports/ScissorRects hooks upscale the vrcam blit's
    // render-res (1418) viewport to the output (2444). Set OUTSIDE the SEH block (POD write) and
    // only for the vrcam eval. Cleared at the command-list Reset that starts the next frame.
    if (flipped && CyberpunkVR_VrcamDlssScale)
        t_vrcam_dlss_post = true;
}

// Did MAIN's ApplyDLSS have flag 0x45 set (i.e. is the game actually using DLSS)? Observed
// each frame from main's ApplyDLSS call; vrcam only mirrors it when true. This makes the
// feature a strict MIRROR of main's upscaler: if the user has DLSS OFF (main lacks flag
// 0x45, or the ApplyDLSS node isn't even emitted), vrcam is never forced into DLSS.
static bool g_main_dlss_flag = false;

// ApplyDLSS node work-fn: mirror main's DLSS onto vrcam. For main (key 0) we OBSERVE flag
// 0x45; for vrcam we SET it (only if main has it) so vrcam takes the full eval path AND all
// POST-DLSS nodes (tonemap/bloom sub_140769308 read flags 0x45/0x47/0x48/0x49 to pick the
// DLSS-output source+dims). CRITICAL: main keeps 0x45 set the WHOLE frame; the tonemap is a
// SEPARATE node that runs after ApplyDLSS. Restoring 0x45 right after ApplyDLSS left vrcam's
// tonemap seeing 0x45=0 -> it read the wrong (pre-DLSS, since-aliased placed) source -> the
// top band flickered as that memory got reused. So set-and-KEEP it persistently (like main,
// same lesson as the IPD write); clear only when main drops DLSS or VrcamDlss is toggled off
// (so vrcam never gets stuck in a DLSS path with no eval behind it). NEVER forces DLSS on.
static __int64 __fastcall Detour_ApplyDlss(void* a1, void* a2) {
    if (a2) {
        __try {
            uintptr_t ctx = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(a2) + 0x18);
            if (ctx) {
                const uint64_t key = *reinterpret_cast<uint64_t*>(ctx + 0x28);
                const uintptr_t q = ctx + DLSS_FLAGSET_OFF;
                if (key == 0) {
                    g_main_dlss_flag = (*reinterpret_cast<uint64_t*>(q) & DLSS_EVAL_FLAG_BIT) != 0;
                } else if (key == g_vrcam_ctx_key) {
                    // want the flag set iff the feature is enabled AND main is actually on DLSS
                    const bool want = (CyberpunkVR_VrcamDlss != 0) && g_main_dlss_flag;
                    const uint64_t cur = *reinterpret_cast<uint64_t*>(q);
                    if (want && !(cur & DLSS_EVAL_FLAG_BIT))
                        *reinterpret_cast<uint64_t*>(q) = cur | DLSS_EVAL_FLAG_BIT;   // set & keep
                    else if (!want && (cur & DLSS_EVAL_FLAG_BIT))
                        *reinterpret_cast<uint64_t*>(q) = cur & ~DLSS_EVAL_FLAG_BIT;  // unstick
                    ++CyberpunkVR_DebugVrcamApplyDlssHits;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return g_orig_applydlss(a1, a2);
}

// PATH-A graph-level experiment: skip the DLSS-gated salt70 post-color node for the
// vrcam view. Mirrors the node's own view/id computation: view = (a2[6]&1)?0:a2[13];
// id = (view<<24)^0x3D7E6258; vrcam id = 0x3C7E6258. When skipped we do NOT call the
// original, removing its type-4 post-color write (and its readback) from the vrcam graph.

// ===== VRCAM DLSS render-res downscale: make vrcam UPSCALE instead of DLAA =====
// Root cause of vrcam-DLAA (found via HW-write BP on the vrcam view's render-res field):
// sub_1404E42A0 computes each view's DLSS render resolution. Its a1 == &view[0x34] (the
// render-res sub-struct): renderW@+0, renderH@+4, prevW/H@+8/12, dupW@+16/+20, dupH@+24/+28,
// targetW@+32, targetH@+36, accum@+48. For the MAIN view the engine scales target x DLSS-scale
// (~0.58 Balanced, via renderer vtable+1080) because its DLSS flag (bit 0x20 @ view+0x17D8) is
// set; the VRCAM view's flag is NOT set yet at render-setup (ApplyDLSS sets it later in the
// frame) so vrcam falls through to render==target (1:1 == DLAA).
// FIX: after the engine computes vrcam's 1:1 res, overwrite it with round(target x scale) so
// vrcam's scene renders at ~58% into its 2444^2 RTs and DLSS upscales to 2444^2 (real FPS).
// This mirrors the engine's own scaled branch exactly (verified live: main 1920x1080 ->
// 1114x627 == x0.580175). Scale is read from the SHARED DLSS-state (renderer+0x4658)+0x400 ==
// the value main uses, so vrcam auto-mirrors main's quality mode (DLAA->skip, Balanced->0.58,
// Performance->0.5). Discriminator = view identity (CName "vrcam" @ view+0x28), NOT resolution.
// Gated by VrcamDlss (only when vrcam is on DLSS) + dedicated toggle CyberpunkVR_VrcamDlssScale.
constexpr uintptr_t RENDER_RES_RVA = 0x4E42A0;   // sub_1404E42A0 per-view DLSS render-res compute
using RenderResFn = __int64(__fastcall*)(void*, void*, void*);
static RenderResFn g_orig_render_res = nullptr;
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_VrcamDlssScale         = 1;  // RETIRED/vestigial: core upscale + crop-fix now key off VrcamDlss ALONE (see Detour_RenderRes). FlagCompute drives the native downscale from VrcamDlss. Kept default=1 for overlay/back-compat (only gates dormant STAGE2 / diagnostics now).
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_VrcamClearFlag64       = 1;  // 1=clear vrcam build-flag 64 (bit0 view+0x17D8) -> build output-res post like main (crop fix)
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugClearFlag64Hits   = 0;  // times flag64 cleared for vrcam
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugVrcamResScaleHits = 0;  // times vrcam res was scaled
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugVrcamRenderW      = 0;  // last vrcam render width (diag)
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_DebugVrcamRenderH      = 0;  // last vrcam render height (diag)
// COMPUTE-RESOLVE ROUTING (plan B, live-tunable): the vrcam-only crop = raster tonemap
// sub_140768510, gated by GROUP 20 = bit20 of view+0x17D0 (sub_14023AF5C(ctx,20)). LIVE-CONFIRMED:
// main bit20=0 (SKIPS the raster tonemap body -> composites via COMPUTE, res-agnostic, full output),
// vrcam bit20=1 (runs raster tonemap @ render-res 1418 viewport on the 2444 DLSS output -> CROP).
// The ONLY two view+0x17D0 bits that differ main-vs-vrcam are bit20 (main0/vrcam1) & bit25 (main1/vrcam0).
// Mode: 0=off, 1=clear bit20, 2=clear bit20 + set bit25 (== EXACT match to MAIN), 3=set bit25 only.
// Read at EXECUTE by the tonemap's own gate every frame => NOT the cached build-graph group-69 dead end.
extern "C" __declspec(dllexport) uint32_t CyberpunkVR_VrcamComputeResolve   = 2;  // SHIPPED DEFAULT=2. LIVE-VERDICT: bit20 (group 20) is necessary+sufficient for the crop (clear=no crop, set=crop; mode 3 proved bit25 alone irrelevant). 2 = EXACT match to MAIN's view+0x17D0 (a config main runs every frame => known-good, no synthetic hybrid). Active under VrcamDlss ALONE (VrcamDlssScale retired); DLAA/no-DLSS: matching main's flags is a no-op there.
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugP17D0Hits        = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugP17D0Before      = 0;  // view+0x17D0 before our edit (diag)
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugP17D0After       = 0;  // view+0x17D0 after our edit (diag)

// ===== POST-DLSS CROP FIX: command-list viewport/scissor correction (slots 21/22/10) =====
// The vrcam-only post-DLSS blit (PipelineState_563) sets a render-res (1418) viewport+scissor on
// the 2444 DLSS-output RT -> top-left crop. We upscale that viewport/scissor to the vrcam output
// (2444) but ONLY: (a) this command-list thread is in the post-DLSS phase (t_vrcam_dlss_post, set
// at the vrcam DLSS eval on this same thread, cleared at the next Reset), and (b) the rect is
// exactly the vrcam RENDER size (main never uses the square vrcam render size, so main is never
// touched; pre-DLSS vrcam passes run before the eval so the phase flag is still false for them).
// CROP-PASS gate: viewport is the vrcam RENDER size (1418) while the RT bound on THIS thread is the
// vrcam OUTPUT size (2444) => the 563 blit under-filling the 2444 target. Thread-agnostic (t_cur_rt
// captured in hk_OMSetRenderTargets on the same recording thread just before this call).
// CROP-PASS gate (thread-independent, RTV-free): viewport width == the vrcam RENDER size (1418)
// while DLSS-upscale is active. Main never uses the square vrcam render size, so main is never
// matched. This catches BOTH pre-DLSS vrcam scene passes AND the post-DLSS crop pass; we separate
// them offline by intersecting the captured node RVAs with the readers of post-color 0x3D7E6258.
// (removed: vrcam_render_res_viewport helper -- only used by the now-pass-through viewport/scissor hooks)
static void STDMETHODCALLTYPE hk_RSSetViewports(
        ID3D12GraphicsCommandList* self, UINT count, const D3D12_VIEWPORT* vps) {
    // Pass-through. The post-DLSS crop is fixed natively in Detour_RenderRes (view+0x17D0 match-main);
    // the old render-res-viewport band-aid + per-frame stack-capture diagnostics were removed.
    const CommandListVtableHook* e = command_list_hook_entry(self);
    PFN_RSSetViewports orig = e ? e->viewports_original : nullptr;
    if (orig) orig(self, count, vps);
}
static void STDMETHODCALLTYPE hk_RSSetScissorRects(
        ID3D12GraphicsCommandList* self, UINT count, const D3D12_RECT* rects) {
    // Pass-through (see hk_RSSetViewports).
    const CommandListVtableHook* e = command_list_hook_entry(self);
    PFN_RSSetScissorRects orig = e ? e->scissor_original : nullptr;
    if (orig) orig(self, count, rects);
}
static HRESULT STDMETHODCALLTYPE hk_GfxReset(
        ID3D12GraphicsCommandList* self, ID3D12CommandAllocator* alloc,
        ID3D12PipelineState* pso) {
    t_vrcam_dlss_post = false;
    t_cur_rt_w = 0; t_cur_rt_h = 0;   // stale RT cleared at frame-start recording
    // A reset ends the recording that owned any pending HUD bind. Dropping it here means the
    // snapshot can never barrier a resource whose render-target state belonged to a list that
    // no longer exists.
    t_hud_rt_bound = nullptr; t_hud_rt_list = nullptr;
    const CommandListVtableHook* e = command_list_hook_entry(self);
    PFN_GfxReset orig = e ? e->reset_original : nullptr;
    return orig ? orig(self, alloc, pso) : S_OK;
}

// EXPERIMENT (attempt 2): overriding only the render-res struct was DISPROVEN live -- the
// whole struct (view+0x34..) went to 1418 yet the vrcam SCENE still rendered 2444 (DLSS then
// cropped the 1418 sub-rect of a full-2444 image => ZOOM). So the pass rasterizer viewport is
// NOT driven by the render-res value; it is gated by the DLSS/dynamic-res FLAG itself
// (bit 0x20 == flag 0x45 @ view+0x17D8). MAIN has that flag set for the whole frame (=> scaled
// render-res AND dynamic-res viewport); vrcam's is cleared by the per-frame view reset and only
// re-set LATE by ApplyDLSS. Attempt 2: set the flag EARLY (before g_orig, at the earliest
// per-frame setup hook we own) so vrcam takes main's full dynamic-res path. Still gated by the
// toggle (default OFF until proven); belt-and-suspenders render-res override kept for determinism.
static __int64 __fastcall Detour_RenderRes(void* a1, void* a2, void* a3) {
    bool vrcam = false;
    // CONSOLIDATED: gate the whole vrcam upscale + crop-fix path on VrcamDlss ALONE. VrcamDlssScale
    // is retired -- Detour_FlagCompute (VrcamDlss-gated) forces the DLSS upscaler group at graph
    // build, so the engine's render-res writer (g_orig below) downscales vrcam natively from VrcamDlss
    // alone; the separate "upscale" toggle is no longer needed. The downscale override below still
    // self-guards on main's actual DLSS scale (0.30..0.999), so DLAA / no-DLSS stay 1:1.
    if (CyberpunkVR_VrcamDlss && a1) {
        __try {
            uint8_t* view = reinterpret_cast<uint8_t*>(a1) - 0x34;   // a1 == &view[0x34]
            if (*reinterpret_cast<uint64_t*>(view + 0x28) == g_vrcam_ctx_key) {
                vrcam = true;
                // set the master DLSS/dynamic-res flag EARLY so g_orig scales AND the later
                // pass-viewport setup takes the dynamic-res path (like main), for this frame.
                *reinterpret_cast<uint64_t*>(view + DLSS_FLAGSET_OFF) |= DLSS_EVAL_FLAG_BIT;
                // ROOT-CAUSE FIX (build-time flag gate): flag 64 (bit0 of view+0x17D8) is the SOLE
                // main/vrcam view-flag difference (main=0, vrcam=1). The frame-graph SCENE_FULL
                // builder (sub_141D43040) tests it via sub_1407305B0(ctx+0x17D0, N) at BUILD time to
                // decide the post/final chain. Clearing it -> vrcam's graph is built like MAIN's
                // (output-res post declarations) while the scene still renders downscaled (below)
                // => native DLSS upscale, no crop. Detour_RenderRes runs pre-build so the clear is
                // seen by the builder. A/B via CyberpunkVR_VrcamClearFlag64.
                if (CyberpunkVR_VrcamClearFlag64) {
                    *reinterpret_cast<uint64_t*>(view + DLSS_FLAGSET_OFF) &= ~1ULL;
                    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugClearFlag64Hits));
                }
                // PLAN B (live-tunable): match MAIN's view+0x17D0 group flags so vrcam SKIPS the
                // raster tonemap (group 20) and composites via the COMPUTE path like main -> no crop.
                if (CyberpunkVR_VrcamComputeResolve) {
                    uint64_t* p = reinterpret_cast<uint64_t*>(view + 0x17D0);
                    uint64_t before = *p, nv = before;
                    switch (CyberpunkVR_VrcamComputeResolve) {
                        case 1: nv &= ~(1ull << 20); break;                    // clear group 20
                        case 2: nv = (nv & ~(1ull << 20)) | (1ull << 25); break; // match MAIN exactly
                        case 3: nv |= (1ull << 25); break;                    // set group 25 only
                        default: break;
                    }
                    *p = nv;
                    CyberpunkVR_DebugP17D0Before = before;
                    CyberpunkVR_DebugP17D0After  = nv;
                    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugP17D0Hits));
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { vrcam = false; }
    }
    __int64 r = g_orig_render_res(a1, a2, a3);
    if (vrcam && a1) {
        __try {
            float scale = 0.0f;
            uintptr_t renderer = *reinterpret_cast<uintptr_t*>(g_exe_base + RENDERER_GLOBAL_RVA);
            if (renderer) {
                uintptr_t dlss = *reinterpret_cast<uintptr_t*>(renderer + OFF_VIEWSTATE);
                if (dlss) scale = *reinterpret_cast<float*>(dlss + 0x400);
            }
            int32_t* p = reinterpret_cast<int32_t*>(a1);
            int32_t tW = p[8];    // a1+32 target W
            int32_t tH = p[9];    // a1+36 target H
            // only when main is actually upscaling (skip DLAA / insane values)
            if (scale > 0.30f && scale < 0.999f && tW > 0 && tH > 0) {
                int32_t rW = static_cast<int32_t>(static_cast<float>(tW) * scale + 0.5f);
                int32_t rH = static_cast<int32_t>(static_cast<float>(tH) * scale + 0.5f);
                if (rW < 1) rW = 1;
                if (rH < 1) rH = 1;
                p[0] = rW; p[4] = rW; p[5] = rW;    // renderW: a1+0, a1+16, a1+20
                p[1] = rH; p[6] = rH; p[7] = rH;    // renderH: a1+4, a1+24, a1+28
                *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(a1) + 48) = 0; // reset accum (engine scaled branch does this)
                CyberpunkVR_DebugVrcamRenderW = static_cast<uint32_t>(rW);
                CyberpunkVR_DebugVrcamRenderH = static_cast<uint32_t>(rH);
                // publish vrcam render+output dims for the DLSS const/eval subrect+MV fix
                g_vrcam_dlss_rw = rW; g_vrcam_dlss_rh = rH;
                g_vrcam_dlss_ow = tW; g_vrcam_dlss_oh = tH;
                InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&CyberpunkVR_DebugVrcamResScaleHits));
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return r;
}

static void install_desc_ring_probe() {
    if (g_desc_ring_probe_installed) return;
    if (!g_exe_base) sync_stereo_init();
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        log("[install] MH_Initialize failed: %d", (int)st); return;
    }
    // Frame-graph build observers (DLSS upscaler-group capture/force backup for the crop fix).
    void* fb = reinterpret_cast<void*>(g_exe_base + FULL_BUILD_RVA);
    if (MH_CreateHook(fb, (void*)&Detour_FullBuild, (void**)&g_orig_full_build) == MH_OK &&
        MH_EnableHook(fb) == MH_OK) log("[build] full-build sub_141D43040 hooked @%p", fb);
    else log("[build] failed to hook full-build @%p", fb);
    void* ib = reinterpret_cast<void*>(g_exe_base + INCR_BUILD_RVA);
    if (MH_CreateHook(ib, (void*)&Detour_IncrBuild, (void**)&g_orig_incr_build) == MH_OK &&
        MH_EnableHook(ib) == MH_OK) log("[build] incr-build sub_141D475B0 hooked @%p", ib);
    else log("[build] failed to hook incr-build @%p", ib);
    g_handle_assign = reinterpret_cast<HandleAssignFn>(g_exe_base + HANDLE_ASSIGN_RVA);
    // DLSS-for-vrcam group force + crop fix (view+0x17D0).
    void* fc = reinterpret_cast<void*>(g_exe_base + FLAG_COMPUTE_RVA);
    if (MH_CreateHook(fc, (void*)&Detour_FlagCompute, (void**)&g_orig_flag_compute) == MH_OK &&
        MH_EnableHook(fc) == MH_OK) log("[flagforce] flag-compute sub_141D49540 hooked @%p", fc);
    else log("[flagforce] failed to hook flag-compute @%p", fc);
    void* rc = reinterpret_cast<void*>(g_exe_base + RECT_COMPUTE_RVA);
    if (MH_CreateHook(rc, (void*)&Detour_RectCompute, (void**)&g_orig_rect_compute) == MH_OK &&
        MH_EnableHook(rc) == MH_OK) log("[lighting] rect-compute sub_1404E3EB4 hooked @%p", rc);
    else log("[lighting] failed to hook rect-compute @%p", rc);
    // VRCAM reuse optimizations (distant shadows / local shadows / GI).
    {
        void* cnd = reinterpret_cast<void*>(g_exe_base + CLOUDS_NODE_RVA);
        if (MH_CreateHook(cnd, (void*)&Detour_CloudsNode, (void**)&g_orig_clouds_node) == MH_OK &&
            MH_EnableHook(cnd) == MH_OK)
            log("[cloudnode] clouds node sub_14061B5B4 hooked @%p", cnd);
        else
            log("[cloudnode] failed to hook the clouds node @%p", cnd);
    }
    {
        void* skw = reinterpret_cast<void*>(g_exe_base + SKY_WORK_RVA);
        if (MH_CreateHook(skw, (void*)&Detour_SkyWork, (void**)&g_orig_sky_work) == MH_OK &&
            MH_EnableHook(skw) == MH_OK)
            log("[sky] sky work sub_1407818F8 hooked @%p (SkyReuseMode=%u)",
                skw, CyberpunkVR_SkyReuseMode);
        else
            log("[sky] failed to hook sky work @%p -- both views keep filling one sky", skw);
    }
    void* dwr = reinterpret_cast<void*>(g_exe_base + DISTANT_RENDER_RVA);
    if (MH_CreateHook(dwr, (void*)&Detour_DistantRender, (void**)&g_orig_distant_render) == MH_OK &&
        MH_EnableHook(dwr) == MH_OK) log("[distant] distant-render sub_140373998 hooked @%p", dwr);
    else log("[distant] failed to hook distant-render @%p", dwr);
    void* dwp = reinterpret_cast<void*>(g_exe_base + DISTANT_PREPARE_RVA);
    if (MH_CreateHook(dwp, (void*)&Detour_DistantPrepare, (void**)&g_orig_distant_prepare) == MH_OK &&
        MH_EnableHook(dwp) == MH_OK) log("[distant] distant-prepare sub_140374AD8 hooked @%p", dwp);
    else log("[distant] failed to hook distant-prepare @%p", dwp);
    void* lbf = reinterpret_cast<void*>(g_exe_base + LIGHTBUFFERS_RVA);
    if (MH_CreateHook(lbf, (void*)&Detour_LightBuffers, (void**)&g_orig_lightbuffers) == MH_OK &&
        MH_EnableHook(lbf) == MH_OK)
        log("[light] RenderLightBuffers sub_14077D308 hooked @%p (block-list lend)", lbf);
    else log("[light] failed to hook RenderLightBuffers @%p", lbf);
    void* ccb = reinterpret_cast<void*>(g_exe_base + CLOUD_CB_FILL_RVA);
    if (MH_CreateHook(ccb, (void*)&Detour_CloudCbFill, (void**)&g_orig_cloud_cb) == MH_OK &&
        MH_EnableHook(ccb) == MH_OK) log("[clouds] cloud-CB sub_140784654 hooked @%p", ccb);
    else log("[clouds] failed to hook cloud-CB @%p", ccb);
    void* lsm = reinterpret_cast<void*>(g_exe_base + LOCAL_SHADOW_RVA);
    if (MH_CreateHook(lsm, (void*)&Detour_LocalShadowMaps, (void**)&g_orig_local_shadow) == MH_OK &&
        MH_EnableHook(lsm) == MH_OK) log("[localshadow] local-shadow sub_140AD5770 hooked @%p", lsm);
    else log("[localshadow] failed to hook local-shadow @%p", lsm);
    void* cbup = reinterpret_cast<void*>(g_exe_base + CB_UPLOAD_RVA);
    if (MH_CreateHook(cbup, (void*)&Detour_CbUpload, (void**)&g_orig_cb_upload) == MH_OK &&
        MH_EnableHook(cbup) == MH_OK)
        log("[gradeup] constant uploader sub_1401EE3CC hooked @%p", cbup);
    else log("[gradeup] failed to hook constant uploader @%p", cbup);
    void* gradec = reinterpret_cast<void*>(g_exe_base + GRADING_COMPOSE_RVA);
    if (MH_CreateHook(gradec, (void*)&Detour_GradingCompose,
                      (void**)&g_orig_grading_compose) == MH_OK &&
        MH_EnableHook(gradec) == MH_OK)
        log("[grade] grading composer sub_14077B538 hooked @%p", gradec);
    else log("[grade] failed to hook grading composer @%p", gradec);
    void* tml = reinterpret_cast<void*>(g_exe_base + TONEMAP_LUT_RVA);
    if (MH_CreateHook(tml, (void*)&Detour_TonemapLut, (void**)&g_orig_tonemap_lut) == MH_OK &&
        MH_EnableHook(tml) == MH_OK)
        log("[grading] GenerateTonemappingLUT sub_140EFC110 hooked @%p (vrcam borrows main's grading source)", tml);
    else log("[grading] failed to hook GenerateTonemappingLUT @%p", tml);
    void* gin = reinterpret_cast<void*>(g_exe_base + GI_NODE_RVA);
    if (MH_CreateHook(gin, (void*)&Detour_GiNode, (void**)&g_orig_gi_node) == MH_OK &&
        MH_EnableHook(gin) == MH_OK) log("[gi] GI-node sub_14077E664 hooked @%p", gin);
    else log("[gi] failed to hook GI-node @%p", gin);
    void* gcp = reinterpret_cast<void*>(g_exe_base + GRAPH_CONTEXT_PREPARE_RVA);
    if (MH_CreateHook(gcp, (void*)&Detour_GraphContextPrepare,
            (void**)&g_orig_graph_context_prepare) == MH_OK &&
        MH_EnableHook(gcp) == MH_OK)
        log("[cull] GraphContextPrepare sub_14079ACA0 hooked @%p", gcp);
    else log("[cull] failed to hook GraphContextPrepare @%p", gcp);
    void* gcr = reinterpret_cast<void*>(g_exe_base + GRAPH_CONTEXT_RESET_RVA);
    if (MH_CreateHook(gcr, (void*)&Detour_GraphContextReset,
            (void**)&g_orig_graph_context_reset) == MH_OK &&
        MH_EnableHook(gcr) == MH_OK)
        log("[cull] GraphContextReset sub_14079C05C hooked @%p", gcr);
    else log("[cull] failed to hook GraphContextReset @%p", gcr);
    void* vcol = reinterpret_cast<void*>(g_exe_base + VIS_COLLECTOR_RVA);
    if (MH_CreateHook(vcol, (void*)&Detour_VisibilityCollector,
            (void**)&g_orig_visibility_collector) == MH_OK &&
        MH_EnableHook(vcol) == MH_OK)
        log("[cull] VisibilityCollector sub_14079CB6C hooked @%p", vcol);
    else log("[cull] failed to hook VisibilityCollector @%p", vcol);
    void* mwrk = reinterpret_cast<void*>(g_exe_base + MATERIALIZE_WORKER_RVA);
    if (MH_CreateHook(mwrk, (void*)&Detour_MaterializeWorker,
            (void**)&g_orig_materialize_worker) == MH_OK &&
        MH_EnableHook(mwrk) == MH_OK)
        log("[cull] MaterializeWorker sub_14036DDC4 hooked @%p", mwrk);
    else log("[cull] failed to hook MaterializeWorker @%p", mwrk);
    void* mcp = reinterpret_cast<void*>(g_exe_base + MAIN_CULL_PREP_RVA);
    if (MH_CreateHook(mcp, (void*)&Detour_MainCullPrepare,
            (void**)&g_orig_main_cull_prepare) == MH_OK &&
        MH_EnableHook(mcp) == MH_OK)
        log("[cull] MainCullPrepare sub_14062463C hooked @%p", mcp);
    else log("[cull] failed to hook MainCullPrepare @%p", mcp);
    void* gci = reinterpret_cast<void*>(g_exe_base + MAIN_CULL_CTX_INIT_RVA);
    if (MH_CreateHook(gci, (void*)&Detour_GatherCtxInit,
            (void**)&g_orig_gather_ctx_init) == MH_OK &&
        MH_EnableHook(gci) == MH_OK)
        log("[cull] GatherCtxInit sub_140623FD8 hooked @%p (LOD-thresh sweep)", gci);
    else log("[cull] failed to hook GatherCtxInit @%p", gci);
    void* vqp = reinterpret_cast<void*>(g_exe_base + VIS_QUERY_PREPARE_RVA);
    if (MH_CreateHook(vqp, (void*)&Detour_VisQueryPrepare,
            (void**)&g_orig_visquery_prepare) == MH_OK &&
        MH_EnableHook(vqp) == MH_OK)
        log("[cull] VisQueryPrepare sub_14079E50C hooked @%p (occlusion gate)", vqp);
    else log("[cull] failed to hook VisQueryPrepare @%p", vqp);
    void* mct = reinterpret_cast<void*>(g_exe_base + MAIN_CULL_TEST_RVA);
    if (MH_CreateHook(mct, (void*)&Detour_MainCullTest,
            (void**)&g_orig_main_cull_test) == MH_OK &&
        MH_EnableHook(mct) == MH_OK)
        log("[cull] MainCullTest sub_140624694 hooked @%p", mct);
    else log("[cull] failed to hook MainCullTest @%p", mct);
    void* fmat = reinterpret_cast<void*>(g_exe_base + FINE_MATERIALIZE_RVA);
    if (MH_CreateHook(fmat, (void*)&Detour_FineMaterialize,
            (void**)&g_orig_fine_materialize) == MH_OK &&
        MH_EnableHook(fmat) == MH_OK)
        log("[cull] FineMaterialize sub_14014DFE8 hooked @%p", fmat);
    else log("[cull] failed to hook FineMaterialize @%p", fmat);
    void* vapp = reinterpret_cast<void*>(g_exe_base + VISIBLE_APPEND_RVA);
    if (MH_CreateHook(vapp, (void*)&Detour_VisibleAppend,
            (void**)&g_orig_visible_append) == MH_OK &&
        MH_EnableHook(vapp) == MH_OK)
        log("[cull] VisibleAppend sub_140109A44 hooked @%p", vapp);
    else log("[cull] failed to hook VisibleAppend @%p", vapp);
    void* pstg = reinterpret_cast<void*>(g_exe_base + PREPARE_STAGE_RVA);
    if (MH_CreateHook(pstg, (void*)&Detour_PrepareStage,
            (void**)&g_orig_prepare_stage) == MH_OK &&
        MH_EnableHook(pstg) == MH_OK)
        log("[prep] stage sub_141D57210 hooked @%p", pstg);
    else log("[prep] failed to hook stage @%p", pstg);
    void* pgat = reinterpret_cast<void*>(g_exe_base + PREPARE_GATHER_RVA);
    if (MH_CreateHook(pgat, (void*)&Detour_PrepareGather,
            (void**)&g_orig_prepare_gather) == MH_OK &&
        MH_EnableHook(pgat) == MH_OK)
        log("[prep] gather sub_14015375C hooked @%p", pgat);
    else log("[prep] failed to hook gather @%p", pgat);
    void* pflt = reinterpret_cast<void*>(g_exe_base + PREPARE_FILTER_RVA);
    if (MH_CreateHook(pflt, (void*)&Detour_PrepareFilter,
            (void**)&g_orig_prepare_filter) == MH_OK &&
        MH_EnableHook(pflt) == MH_OK)
        log("[prep] filter sub_141D57100 hooked @%p", pflt);
    else log("[prep] failed to hook filter @%p", pflt);
    void* pfin = reinterpret_cast<void*>(g_exe_base + PREPARE_FINALIZE_RVA);
    if (MH_CreateHook(pfin, (void*)&Detour_PrepareFinalize,
            (void**)&g_orig_prepare_finalize) == MH_OK &&
        MH_EnableHook(pfin) == MH_OK)
        log("[prep] finalize sub_140379568 hooked @%p", pfin);
    else log("[prep] failed to hook finalize @%p", pfin);
    void* psa = reinterpret_cast<void*>(g_exe_base + PREPARE_SORT_A_RVA);
    if (MH_CreateHook(psa, (void*)&Detour_PrepareSortA,
            (void**)&g_orig_prepare_sort_a) == MH_OK && MH_EnableHook(psa) == MH_OK)
        log("[prep] sortA sub_14037A54C hooked @%p", psa);
    else log("[prep] failed to hook sortA @%p", psa);
    void* psb = reinterpret_cast<void*>(g_exe_base + PREPARE_SORT_B_RVA);
    if (MH_CreateHook(psb, (void*)&Detour_PrepareSortB,
            (void**)&g_orig_prepare_sort_b) == MH_OK && MH_EnableHook(psb) == MH_OK)
        log("[prep] sortB sub_14037A984 hooked @%p", psb);
    else log("[prep] failed to hook sortB @%p", psb);
    void* psc = reinterpret_cast<void*>(g_exe_base + PREPARE_SORT_C_RVA);
    if (MH_CreateHook(psc, (void*)&Detour_PrepareSortC,
            (void**)&g_orig_prepare_sort_c) == MH_OK && MH_EnableHook(psc) == MH_OK)
        log("[prep] sortC sub_14037ADB4 hooked @%p", psc);
    else log("[prep] failed to hook sortC @%p", psc);
    void* psf = reinterpret_cast<void*>(g_exe_base + PREPARE_SORT_FINAL_RVA);
    if (MH_CreateHook(psf, (void*)&Detour_PrepareSortFinal,
            (void**)&g_orig_prepare_sort_final) == MH_OK && MH_EnableHook(psf) == MH_OK)
        log("[prep] finalSort sub_14045E33C hooked @%p", psf);
    else log("[prep] failed to hook finalSort @%p", psf);
    void* dcl = reinterpret_cast<void*>(g_exe_base + DOCULLING_RVA);
    if (MH_CreateHook(dcl, (void*)&Detour_DoCulling, (void**)&g_orig_doculling) == MH_OK &&
        MH_EnableHook(dcl) == MH_OK) log("[cull] DoCulling sub_140B2BEFC hooked @%p", dcl);
    else log("[cull] failed to hook DoCulling @%p", dcl);
    void* qwk = reinterpret_cast<void*>(g_exe_base + QUERYWORK_RVA);
    if (MH_CreateHook(qwk, (void*)&Detour_QueryWork, (void**)&g_orig_querywork) == MH_OK &&
        MH_EnableHook(qwk) == MH_OK) log("[cull] QueryWork sub_14014D03C hooked @%p", qwk);
    else log("[cull] failed to hook QueryWork @%p", qwk);
    void* dcmp = reinterpret_cast<void*>(g_exe_base + DRAWCOMP_RVA);
    if (MH_CreateHook(dcmp, (void*)&Detour_DrawComposition, (void**)&g_orig_drawcomp) == MH_OK &&
        MH_EnableHook(dcmp) == MH_OK) log("[cull] DrawComposition sub_14020A264 hooked @%p", dcmp);
    else log("[cull] failed to hook DrawComposition @%p", dcmp);
    // Stereo/IPD + temporal-history via SetStreamlineConstants.
    void* sl = reinterpret_cast<void*>(g_exe_base + SL_CONSTANTS_RVA);
    if (MH_CreateHook(sl, (void*)&Detour_SlConstants, (void**)&g_orig_sl_const) == MH_OK &&
        MH_EnableHook(sl) == MH_OK) log("[sl] SetStreamlineConstants sub_140788A9C hooked @%p", sl);
    else log("[sl] failed to hook SetStreamlineConstants @%p", sl);
    // DLSS-for-vrcam drivers (own Streamline viewport + eval + apply).
    void* dc = reinterpret_cast<void*>(g_exe_base + DLSS_CONST_RVA);
    if (MH_CreateHook(dc, (void*)&Detour_DlssConst, (void**)&g_orig_dlss_const) == MH_OK &&
        MH_EnableHook(dc) == MH_OK) log("[dlss] constants driver sub_14078933C hooked @%p", dc);
    else log("[dlss] failed to hook constants driver @%p", dc);
    void* de = reinterpret_cast<void*>(g_exe_base + DLSS_EVAL_RVA);
    if (MH_CreateHook(de, (void*)&Detour_DlssEval, (void**)&g_orig_dlss_eval) == MH_OK &&
        MH_EnableHook(de) == MH_OK) log("[dlss] tag/eval driver sub_141D4FDC0 hooked @%p", de);
    else log("[dlss] failed to hook tag/eval driver @%p", de);
    void* ad = reinterpret_cast<void*>(g_exe_base + APPLYDLSS_WORK_RVA);
    if (MH_CreateHook(ad, (void*)&Detour_ApplyDlss, (void**)&g_orig_applydlss) == MH_OK &&
        MH_EnableHook(ad) == MH_OK) log("[dlss] ApplyDLSS work sub_14037D5C4 hooked @%p", ad);
    else log("[dlss] failed to hook ApplyDLSS work @%p", ad);
    // DLSS-upscale render-res scaler + crop fix (view+0x17D0 match-main).
    void* rr = reinterpret_cast<void*>(g_exe_base + RENDER_RES_RVA);
    if (MH_CreateHook(rr, (void*)&Detour_RenderRes, (void**)&g_orig_render_res) == MH_OK &&
        MH_EnableHook(rr) == MH_OK) log("[dlss] render-res scaler sub_1404E42A0 hooked @%p", rr);
    else log("[dlss] failed to hook render-res scaler @%p", rr);
    // Node dispatcher: drives mirror-copy epilogue + flicker tonemap snapshot + t_current_node_work.
    if (!g_node_dispatch_hooked.load(std::memory_order_acquire)) {
        void* nd = reinterpret_cast<void*>(g_exe_base + NODE_DISPATCH_RVA);
        const MH_STATUS ndSt = MH_CreateHook(nd, (void*)&Detour_NodeDispatch,
                                             (void**)&g_node_dispatch_orig);
        if (ndSt == MH_OK && MH_EnableHook(nd) == MH_OK) {
            g_node_dispatch_hooked.store(true, std::memory_order_release);
            // The proxy reads the view identity from CyberpunkVR_IsMainViewActive(), which
            // this detour maintains, and skips hooking this address itself.
            log("[node] node dispatcher sub_1401EC404 hooked @%p (view key published)", nd);
        } else {
            // Name the status. ALREADY_CREATED means something else took this address first,
            // and the consequence is total: no vrcam node tagging, so no RTV capture, no
            // snapshot, no right eye and no mirror window. That is worth more than "failed".
            log("[node] failed to hook node dispatcher @%p (MH_STATUS=%d%s)", nd, (int)ndSt,
                ndSt == MH_ERROR_ALREADY_CREATED ? " ALREADY_CREATED -- another hook owns it" : "");
        }
    }
    // Per-view capability test -- the gate that keeps the HUD out of the second eye.
    // Hooked unconditionally; the detour itself is a no-op unless CyberpunkVR_HudInVrcam is on
    // AND the second eye is the current view, so leaving it installed costs two loads per call.
    {
        void* vf = reinterpret_cast<void*>(g_exe_base + VIEW_FEATURE_CHECK_RVA);
        const MH_STATUS st = MH_CreateHook(vf, (void*)&Detour_ViewFeatureCheck,
                                           (void**)&g_view_feature_check_orig);
        if (st == MH_OK && MH_EnableHook(vf) == MH_OK) {
            log("[hud] view capability test sub_14021BE28 hooked @%p (HudInVrcam=%d)",
                vf, CyberpunkVR_HudInVrcam);
        } else {
            log("[hud] failed to hook view capability test @%p (MH_STATUS=%d) -- the second eye "
                "keeps the engine's HUD refusal", vf, (int)st);
        }
    }
    // RTT view-create (mirror serial + RTT res) and post-color de-alias (flicker fix).
    void* rv = reinterpret_cast<void*>(g_exe_base + RTT_VIEWCREATE_RVA);
    if (MH_CreateHook(rv, (void*)&Detour_RTTViewCreate, (void**)&g_orig_rtt_viewcreate) == MH_OK &&
        MH_EnableHook(rv) == MH_OK) log("[rtt] view-create sub_1404FBAFC hooked @%p", rv);
    else log("[rtt] failed to hook view-create @%p", rv);
    void* rq = reinterpret_cast<void*>(g_exe_base + RESOLVE_QUERY_RVA);
    if (MH_CreateHook(rq, (void*)&Detour_Resolve3D20, (void**)&g_orig_resolve3d20) == MH_OK &&
        MH_EnableHook(rq) == MH_OK) log("[flicker] post-color de-alias sub_1401F3D20 hooked @%p", rq);
    else log("[flicker] failed to hook post-color de-alias @%p", rq);
    g_desc_ring_probe_installed = true;
    log("[install] engine hooks installed");
}

// Snapshot every toggle that can silently change WHAT the audit measures: a capture
// taken with an experiment armed invalidates the whole table. Lives at the end of the
// anonymous namespace because it reads flags declared throughout the file.
static void prof_log_config() {
    log("[prof] CFG|DistantReuse=%u|LocalShadowReuse=%u|GiReuse=%u|VrcamFlagMode=%u"
        "|OcclusionGateForce=%u|CullReuseMode=%u|NodeCutEnable=%d|NodeCutSkips=%llu"
        "|LodOverride=%u|LodMask=%u|LodValue=%.3f|VrcamDlss=%u|VrcamComputeResolve=%u"
        "|MainUpscalerGroups=0x%03X|MainAaMode=%u|VrcamAaMode=%u|frameMs=%.3f",
        CyberpunkVR_DistantReuseMode, CyberpunkVR_LocalShadowReuseMode,
        CyberpunkVR_GiReuseMode, CyberpunkVR_VrcamFlagMode,
        CyberpunkVR_OcclusionGateForce, CyberpunkVR_CullReuseMode,
        CyberpunkVR_NodeCutEnable,
        (unsigned long long)CyberpunkVR_DebugNodeCutSkips,
        CyberpunkVR_LodThreshOverrideEnable, CyberpunkVR_LodThreshApplyMask,
        CyberpunkVR_LodThreshValue, CyberpunkVR_VrcamDlss,
        CyberpunkVR_VrcamComputeResolve, CyberpunkVR_DebugMainUpscalerGroups,
        CyberpunkVR_DebugMainAaMode, CyberpunkVR_DebugVrcamAaMode,
        CyberpunkVR_ProfFrameMs);
}

} // namespace

// The engine's HUD composite constants, for the eye path in openxr_capture.cpp. Defined out here
// because everything above lives in an anonymous namespace and so has internal linkage.
ColorBlit::HudParams CyberpunkVR_GetHudParams() { return hud_composite_params(); }

void sync_stereo_ensure_descriptor_probe() {
    patch_descriptor_heap_size();
    bool expected = false;
    if (!g_desc_probe_installed.compare_exchange_strong(expected, true)) return;
    if (!g_exe_base) sync_stereo_init();
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    if (!d3d12) d3d12 = LoadLibraryW(L"d3d12.dll");
    if (!d3d12) { log("[descheap] d3d12.dll not present"); return; }
    void* target = reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice"));
    if (!target) { log("[descheap] D3D12CreateDevice not found"); return; }
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        log("[descheap] MH_Initialize failed: %d", (int)st); return;
    }
    if (MH_CreateHook(target, (void*)&Hook_D3D12CreateDevice,
                      (void**)&g_orig_D3D12CreateDevice) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        log("[descheap] failed to hook D3D12CreateDevice @%p", target);
        return;
    }
    log("[descheap] D3D12CreateDevice hook installed @%p (enlarge=%d target=%u)",
        target, (int)g_enable_desc_heap_enlarge, g_desc_heap_target);
}


void sync_stereo_init() {
    load_vrcam_selection();     // before any hook can observe a view
    // Adopts the enabled component's REAL virtualCameraName once CET reports it, so a
    // component whose camera field does not follow the naming convention still works.
    std::thread(vrcam_active_watcher).detach();
    g_exe_base = (uint8_t*)GetModuleHandleW(nullptr);
    QueryPerformanceFrequency(&g_qpc_freq);
    if (g_qpc_freq.QuadPart) g_qpc_to_ms = 1000.0 / (double)g_qpc_freq.QuadPart;
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    g_wait_on_address = reinterpret_cast<WaitOnAddressFn>(
        GetProcAddress(kernelbase, "WaitOnAddress"));
    g_wake_by_address_all = reinterpret_cast<WakeByAddressAllFn>(
        GetProcAddress(kernelbase, "WakeByAddressAll"));
}

void sync_stereo_install_early_hooks() {
    patch_descriptor_heap_size();
    // Install before the game's D3D12CreateDevice so the real device, DIRECT queue and
    // command-list vtables are captured without creating a dummy device through Streamline.
    // As the DXGI proxy we are loaded long before the game touches d3d12, so "early" comes
    // for free here -- in the standalone plugin this was the delicate part.
    sync_stereo_ensure_descriptor_probe();
    install_desc_ring_probe();
    // No overlay install: the plugin build had to raise its own ImGui overlay on a dummy
    // swapchain because a red4ext plugin cannot see Present any other way. The proxy hooks
    // Present directly and draws the panel itself (overlay/imgui_overlay.cpp, Stereo tab).
}


} // namespace cvr
